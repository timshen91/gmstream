//! Headless test client: connects like the web client, receives video and
//! audio for a while, prints timing statistics and writes the bitstream.
//! Run it on the host itself to get exact latencies (same clock).
//!
//!   probe [-1 FLAGS (Hello flags: 1 one video stream, 2 video datagrams)] [-C X,Y (click)] [-k KEYS] [-g PAD_BUTTONS] [-x N (stress reconfiguration)] [-m 0 (no pointer motion)] [-c h264|hevc|av1] [-s WxH] [-r HZ] [-b KBPS] [-t SECONDS] [-o FILE] HOST:PORT

use std::io::Write;
use std::time::{Duration, Instant};

use anyhow::{Result, bail};
use wtransport::{ClientConfig, Endpoint};

fn now_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

fn message(ty: u8, payload: &[u8]) -> Vec<u8> {
    let mut m = vec![ty];
    m.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    m.extend_from_slice(payload);
    m
}

fn pct(v: &mut [f64], p: f64) -> f64 {
    if v.is_empty() {
        return f64::NAN;
    }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[((v.len() - 1) as f64 * p) as usize]
}

#[tokio::main]
async fn main() -> Result<()> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut opt = |flag: &str, default: &str| -> String {
        match args.iter().position(|a| a == flag) {
            Some(i) => {
                let v = args.remove(i + 1);
                args.remove(i);
                v
            }
            None => default.to_owned(),
        }
    };
    let codec: u8 = match opt("-c", "h264").as_str() {
        "h264" => 0,
        "hevc" => 1,
        "av1" => 2,
        _ => bail!("bad codec"),
    };
    let size = opt("-s", "1920x1080");
    let (w, h) = size.split_once('x').unwrap();
    let (w, h): (u16, u16) = (w.parse()?, h.parse()?);
    let refresh: u32 = (opt("-r", "120").parse::<f64>()? * 1000.0) as u32;
    let kbps: u32 = opt("-b", "40000").parse()?;
    let secs: u64 = opt("-t", "5").parse()?;
    let out = opt("-o", "");
    let wiggle = opt("-m", "1") != "0";
    // -1 FLAGS: Hello flags, 1 = all video on one stream (as WebKit clients
    // do), 2 = video as datagrams.
    let flags: u8 = opt("-1", "0").parse()?;
    let one_stream = flags == 1;
    // -x N: every 300 ms alternate bitrate, size (full/half) and refresh (as is/30 Hz), N times.
    let stress: u32 = opt("-x", "0").parse()?;
    // -X MASK: what -x changes: 1 bitrate, 2 size, 4 refresh (default all).
    let mask: u32 = opt("-X", "7").parse()?;
    // -k CODES: tap these evdev keys (comma-separated) one by one, 600 ms apart.
    let keys: Vec<u16> = opt("-k", "").split(',').filter(|s| !s.is_empty()).map(|s| s.parse().unwrap()).collect();
    // -C X,Y: move the pointer to this normalized position and left-click.
    let click: Option<(f32, f32)> = {
        let c = opt("-C", "");
        c.split_once(',').map(|(x, y)| (x.parse().unwrap(), y.parse().unwrap()))
    };
    // -g BUTTONS: tap these standard-gamepad buttons on pad 0, 600 ms apart.
    let pad_buttons: Vec<u32> = opt("-g", "").split(',').filter(|s| !s.is_empty()).map(|s| s.parse().unwrap()).collect();
    let [addr] = &args[..] else { bail!("usage: probe [opts] HOST:PORT") };

    let config = ClientConfig::builder().with_bind_default().with_no_cert_validation().build();
    let endpoint = Endpoint::client(config)?;
    let conn = endpoint.connect(format!("https://{addr}/")).await?;
    let (mut send, mut recv) = conn.open_bi().await?.await?;
    let mut hello = Vec::new();
    hello.extend_from_slice(&2u16.to_le_bytes());
    hello.extend_from_slice(&w.to_le_bytes());
    hello.extend_from_slice(&h.to_le_bytes());
    hello.extend_from_slice(&refresh.to_le_bytes());
    hello.extend_from_slice(&kbps.to_le_bytes());
    hello.push(1 << codec);
    hello.push(codec);
    hello.push(flags);
    send.write_all(&message(0x01, &hello)).await?;

    let mut head = [0u8; 5];
    recv.read_exact(&mut head).await?;
    let mut body = vec![0u8; u32::from_le_bytes(head[1..5].try_into()?) as usize];
    recv.read_exact(&mut body).await?;
    match head[0] {
        0x81 => println!("HelloAck: codec {}", body[0]),
        0x86 => bail!("host error: {}", String::from_utf8_lossy(&body)),
        t => bail!("unexpected message {t:#x}"),
    }
    // Read the rest of the control stream (cursor, lock state) in the background.
    let host_msgs = std::sync::Arc::new(std::sync::Mutex::new(Vec::<String>::new()));
    {
        let host_msgs = host_msgs.clone();
        tokio::spawn(async move {
            loop {
                let mut head = [0u8; 5];
                if recv.read_exact(&mut head).await.is_err() {
                    break;
                }
                let len = u32::from_le_bytes(head[1..5].try_into().unwrap()) as usize;
                let mut body = vec![0u8; len];
                if recv.read_exact(&mut body).await.is_err() {
                    break;
                }
                let note = match head[0] {
                    0x82 => format!(
                        "cursor {}x{} hotspot {},{}",
                        u16::from_le_bytes([body[0], body[1]]),
                        u16::from_le_bytes([body[2], body[3]]),
                        i16::from_le_bytes([body[4], body[5]]),
                        i16::from_le_bytes([body[6], body[7]])
                    ),
                    0x83 => format!("pointer lock {}", body[0]),
                    0x84 => "rumble".to_owned(),
                    _ => continue,
                };
                host_msgs.lock().unwrap().push(note);
            }
        });
    }
    // Wiggle the pointer so gamescope has input to react to.
    tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(800)).await;
        for &k in &keys {
            for pressed in [1u8, 0u8] {
                let mut b = k.to_le_bytes().to_vec();
                b.push(pressed);
                if send.write_all(&message(0x05, &b)).await.is_err() {
                    return;
                }
                tokio::time::sleep(Duration::from_millis(80)).await;
            }
            tokio::time::sleep(Duration::from_millis(520)).await;
        }
        if let Some((x, y)) = click {
            // Approach the spot like a real mouse would.
            for i in (0..10).rev() {
                let mut b = (x - i as f32 * 0.01).to_le_bytes().to_vec();
                b.extend_from_slice(&y.to_le_bytes());
                let _ = send.write_all(&message(0x08, &b)).await;
                tokio::time::sleep(Duration::from_millis(30)).await;
            }
            tokio::time::sleep(Duration::from_millis(300)).await;
            for pressed in [1u8, 0u8] {
                let mut b = 0x110u16.to_le_bytes().to_vec(); // BTN_LEFT
                b.push(pressed);
                let _ = send.write_all(&message(0x06, &b)).await;
                tokio::time::sleep(Duration::from_millis(80)).await;
            }
        }
        let pad = |buttons: u32| {
            let mut b = vec![0u8];
            b.extend_from_slice(&buttons.to_le_bytes());
            b.extend_from_slice(&[0u8; 10]);
            message(0x0a, &b)
        };
        if !pad_buttons.is_empty() {
            // Let Steam pick the new controller up first.
            let _ = send.write_all(&pad(0)).await;
            tokio::time::sleep(Duration::from_millis(2500)).await;
        }
        for &btn in &pad_buttons {
            for bits in [1u32 << btn, 0] {
                if send.write_all(&pad(bits)).await.is_err() {
                    return;
                }
                tokio::time::sleep(Duration::from_millis(80)).await;
            }
            tokio::time::sleep(Duration::from_millis(520)).await;
        }
        for i in 0..stress {
            tokio::time::sleep(Duration::from_millis(300)).await;
            let half = i % 2 == 0;
            let mut b = (10_000 + 7_000 * i).to_le_bytes().to_vec();
            if mask & 1 != 0 && send.write_all(&message(0x04, &b)).await.is_err() {
                return;
            }
            let small = half && mask & 2 != 0;
            let slow = half && mask & 4 != 0;
            b = Vec::new();
            b.extend_from_slice(&(if small { w / 2 } else { w } & !1).to_le_bytes());
            b.extend_from_slice(&(if small { h / 2 } else { h } & !1).to_le_bytes());
            b.extend_from_slice(&(if slow { 30_000 } else { refresh }).to_le_bytes());
            if send.write_all(&message(0x02, &b)).await.is_err() {
                return;
            }
        }
        for i in 0.. {
            if !wiggle {
                std::future::pending::<()>().await;
            }
            let x = (i % 100) as f32 / 100.0;
            let mut p = x.to_le_bytes().to_vec();
            p.extend_from_slice(&0.5f32.to_le_bytes());
            if send.write_all(&message(0x08, &p)).await.is_err() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(8)).await;
        }
    });

    let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<(Vec<u8>, u64)>();
    let audio = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let audio_bytes = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let audio_loud = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
    {
        let (conn, audio, audio_bytes, audio_loud, tx) =
            (conn.clone(), audio.clone(), audio_bytes.clone(), audio_loud.clone(), tx.clone());
        tokio::spawn(async move {
            // Datagram video: frame id → (pieces, pieces received).
            let mut frames: std::collections::HashMap<u32, (Vec<Option<Vec<u8>>>, usize)> = Default::default();
            while let Ok(d) = conn.receive_datagram().await {
                let p = d.payload();
                if p.len() > 9 && p[0] == 2 {
                    let id = u32::from_le_bytes(p[1..5].try_into().unwrap());
                    let index = u16::from_le_bytes(p[5..7].try_into().unwrap()) as usize;
                    let count = u16::from_le_bytes(p[7..9].try_into().unwrap()) as usize;
                    if frames.len() > 16 {
                        let oldest = *frames.keys().min().unwrap();
                        frames.remove(&oldest);
                    }
                    let (parts, got) = frames.entry(id).or_insert_with(|| (vec![None; count], 0));
                    if index < parts.len() && parts[index].is_none() {
                        parts[index] = Some(p[9..].to_vec());
                        *got += 1;
                    }
                    if *got == parts.len() {
                        let (parts, _) = frames.remove(&id).unwrap();
                        let _ = tx.send((parts.into_iter().flatten().flatten().collect(), now_ns()));
                    }
                    continue;
                }
                let opus = d.payload().len().saturating_sub(13);
                audio.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                audio_bytes.fetch_add(opus, std::sync::atomic::Ordering::Relaxed);
                // 5 ms of digital silence encodes to a handful of bytes.
                if opus > 20 {
                    audio_loud.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }
            }
        });
    }

    let mut file = if out.is_empty() { None } else { Some(std::fs::File::create(&out)?) };
    let c2 = conn.clone();
    tokio::spawn(async move {
        if flags == 2 {
            return;
        }
        if one_stream {
            let Ok(mut s) = c2.accept_uni().await else { return };
            loop {
                let mut len = [0u8; 4];
                if s.read_exact(&mut len).await.is_err() {
                    return;
                }
                let mut data = vec![0u8; u32::from_le_bytes(len) as usize];
                if s.read_exact(&mut data).await.is_err() {
                    return;
                }
                let _ = tx.send((data, now_ns()));
            }
        }
        while let Ok(mut s) = c2.accept_uni().await {
            let tx = tx.clone();
            tokio::spawn(async move {
                let mut data = Vec::new();
                let mut buf = vec![0u8; 1 << 16];
                while let Ok(Some(n)) = s.read(&mut buf).await {
                    data.extend_from_slice(&buf[..n]);
                }
                let _ = tx.send((data, now_ns()));
            });
        }
    });

    let start = Instant::now();
    let (mut frames, mut keys, mut bytes, mut gaps) = (0u64, 0u64, 0u64, 0u64);
    let (mut enc, mut net, mut total) = (Vec::new(), Vec::new(), Vec::new());
    let mut last_id: Option<u32> = None;
    let mut dims = (0, 0);
    while start.elapsed() < Duration::from_secs(secs) {
        let Ok(Some((d, recv_ns))) = tokio::time::timeout(Duration::from_secs(1), rx.recv()).await else {
            continue;
        };
        if d.len() < 32 {
            continue;
        }
        let id = u32::from_le_bytes(d[0..4].try_into()?);
        let key = d[4] & 1 != 0;
        dims = (u16::from_le_bytes(d[6..8].try_into()?), u16::from_le_bytes(d[8..10].try_into()?));
        let slot = u64::from_le_bytes(d[12..20].try_into()?);
        let encoded = u64::from_le_bytes(d[20..28].try_into()?);
        if let Some(l) = last_id {
            if id != l.wrapping_add(1) {
                gaps += 1;
            }
        }
        last_id = Some(id);
        frames += 1;
        keys += key as u64;
        bytes += d.len() as u64;
        enc.push((encoded.saturating_sub(slot)) as f64 / 1e6);
        net.push((recv_ns.saturating_sub(encoded)) as f64 / 1e6);
        total.push((recv_ns.saturating_sub(slot)) as f64 / 1e6);
        if let Some(f) = file.as_mut() {
            f.write_all(&d[32..])?;
        }
    }
    let t = start.elapsed().as_secs_f64();
    println!(
        "{frames} frames in {t:.1}s ({:.1} fps), {dims:?}, {keys} keyframes, {gaps} gaps, {:.1} Mbps, {} audio packets",
        frames as f64 / t,
        bytes as f64 * 8.0 / t / 1e6,
        audio.load(std::sync::atomic::Ordering::Relaxed)
    );
    println!(
        "present→encoded  p50 {:.2} ms  p99 {:.2} ms",
        pct(&mut enc, 0.5),
        pct(&mut enc, 0.99)
    );
    println!(
        "encoded→received p50 {:.2} ms  p99 {:.2} ms",
        pct(&mut net, 0.5),
        pct(&mut net, 0.99)
    );
    println!(
        "present→received p50 {:.2} ms  p99 {:.2} ms",
        pct(&mut total, 0.5),
        pct(&mut total, 0.99)
    );
    for m in host_msgs.lock().unwrap().iter() {
        println!("host: {m}");
    }
    let n = audio.load(std::sync::atomic::Ordering::Relaxed).max(1);
    println!(
        "audio: {n} packets, avg {} bytes, {} with sound",
        audio_bytes.load(std::sync::atomic::Ordering::Relaxed) / n,
        audio_loud.load(std::sync::atomic::Ordering::Relaxed)
    );
    conn.close(0u32.into(), b"done");
    endpoint.wait_idle().await;
    Ok(())
}
