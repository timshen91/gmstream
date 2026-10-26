//! gmstream: stream SteamOS Game Mode to a browser.
//!
//! One process: the C core (core/) is the Wayland host gamescope draws into
//! and encodes every frame the moment it is presented; this side serves the
//! web client over HTTPS and talks to it over WebTransport.

mod core;
mod session;
mod web;

use std::process::exit;
use std::sync::{Arc, RwLock};

const USAGE: &str = "usage: gmstream [-p PORT] [-w SOCKET] [-s WIDTHxHEIGHT] [-r HZ] [-S] [-d] [-- command...]
  -p  HTTPS and WebTransport port (default 8443, TCP and UDP)
  -w  Wayland socket name in $XDG_RUNTIME_DIR (default: first free wayland-N)
  -s  initial screen size (default 1920x1080)
  -r  initial refresh rate / VRR maximum (default 120)
  -S  log delivery statistics every second
  -d  debug logging
gamescope connects to the socket (WAYLAND_DISPLAY) and may come and go.
With a command, gmstream runs it with WAYLAND_DISPLAY set and exits with it.";

fn parse_args() -> (u16, core::Config) {
    let mut port = 8443;
    let mut config = core::Config {
        width: 1920,
        height: 1080,
        refresh_mhz: 120_000,
        stats: false,
        debug: false,
        socket: None,
        child: Vec::new(),
    };
    let mut args = std::env::args().skip(1);
    let bad = || -> ! {
        eprintln!("{USAGE}");
        exit(1)
    };
    while let Some(a) = args.next() {
        match a.as_str() {
            "-p" => port = args.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| bad()),
            "-s" => {
                let v = args.next().unwrap_or_else(|| bad());
                let (w, h) = v.split_once('x').unwrap_or_else(|| bad());
                config.width = w.parse().unwrap_or_else(|_| bad());
                config.height = h.parse().unwrap_or_else(|_| bad());
            }
            "-r" => {
                let hz: f64 = args.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| bad());
                config.refresh_mhz = (hz * 1000.0).round() as i32;
            }
            "-S" => config.stats = true,
            "-d" => config.debug = true,
            "-w" => config.socket = Some(args.next().unwrap_or_else(|| bad())),
            "-h" | "--help" => {
                println!("{USAGE}");
                exit(0)
            }
            "--" => {
                config.child = args.by_ref().collect();
            }
            _ => bad(),
        }
    }
    (port, config)
}

fn main() {
    let (port, config) = parse_args();

    // The compositor takes these through signalfd, which only works if no
    // thread can receive them: block them before any thread exists.
    unsafe {
        let mut set: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut set);
        for s in [libc::SIGCHLD, libc::SIGINT, libc::SIGTERM] {
            libc::sigaddset(&mut set, s);
        }
        libc::pthread_sigmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
    }

    let _ = tokio_rustls::rustls::crypto::ring::default_provider().install_default();
    let dir = web::state_dir();
    if let Err(e) = std::fs::create_dir_all(&dir) {
        eprintln!("gmstream: {}: {e}", dir.display());
        exit(1);
    }
    let token = web::load_token(&dir).unwrap_or_else(|e| {
        eprintln!("gmstream: token: {e:#}");
        exit(1)
    });

    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(4)
        .thread_name("gmstream-net")
        .enable_all()
        .build()
        .expect("tokio runtime");
    let hash: session::CertHash = Arc::new(RwLock::new(String::from("[]")));
    {
        let (token, hash) = (token.clone(), hash.clone());
        runtime.spawn(async move {
            if let Err(e) = session::serve(port, token, hash).await {
                eprintln!("gmstream: WebTransport: {e:#}");
                exit(1);
            }
        });
    }
    {
        let hash = hash.clone();
        runtime.spawn(async move {
            if let Err(e) = web::serve(port, dir, hash).await {
                eprintln!("gmstream: HTTPS: {e:#}");
                exit(1);
            }
        });
    }
    eprintln!("gmstream: open https://{}.local:{port}/#{token}", web::hostname());

    // The compositor thread can end up blocked inside the GPU driver (seen
    // with Mesa's VA-API: a spin while holding its lock). Nothing recovers
    // from that in-process, and the process ignores SIGTERM then; abort so the
    // service manager restarts us.
    std::thread::Builder::new()
        .name("gmstream-watchdog".into())
        .spawn(|| loop {
            std::thread::sleep(std::time::Duration::from_secs(2));
            let beat = core::heartbeat_ns();
            if beat != 0 && core::now_ns().saturating_sub(beat) > 10_000_000_000 {
                eprintln!("gmstream: compositor thread stuck for over 10 s, aborting");
                std::process::abort();
            }
        })
        .expect("watchdog thread");

    let code = core::run(&config, &session::HUB);
    exit(code);
}
