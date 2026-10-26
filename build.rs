// Compiles the C core (core/*.c) and links it with the static C libraries
// from the build image (/opt/gm, see build/deps.sh). Only the GPU and audio
// stacks stay dynamic: EGL, GLESv2, GBM and libpipewire come from the host.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const STATIC_PKGS: &[&str] = &[
    "wlroots-0.20",
    "wayland-server",
    "xkbcommon",
    "libavcodec",
    "libavutil",
    "libva-drm",
    "libva",
    "libdrm",
    "opus",
];
const DYNAMIC_PKGS: &[&str] = &["libpipewire-0.3"];

fn pkg_config(args: &[&str], pkgs: &[&str]) -> Vec<String> {
    let out = Command::new("pkg-config")
        .args(args)
        .args(pkgs)
        .output()
        .expect("pkg-config");
    assert!(
        out.status.success(),
        "pkg-config {args:?} {pkgs:?}: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout)
        .unwrap()
        .split_whitespace()
        .map(str::to_owned)
        .collect()
}

fn pkg_var(pkg: &str, var: &str) -> String {
    pkg_config(&[&format!("--variable={var}")], &[pkg]).join("")
}

fn main() {
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let prefix = env::var("GM_PREFIX").unwrap_or_else(|_| "/opt/gm".into());
    println!("cargo:rerun-if-changed=core");
    println!("cargo:rerun-if-env-changed=GM_PREFIX");

    // Protocol headers the wlroots headers include.
    let scanner = pkg_var("wayland-scanner", "wayland_scanner");
    let protocols = pkg_var("wayland-protocols", "pkgdatadir");
    for xml in [
        "stable/xdg-shell/xdg-shell.xml",
        "unstable/pointer-constraints/pointer-constraints-unstable-v1.xml",
        "unstable/xdg-decoration/xdg-decoration-unstable-v1.xml",
    ] {
        let name = Path::new(xml).file_stem().unwrap().to_str().unwrap();
        let status = Command::new(&scanner)
            .args(["server-header", &format!("{protocols}/{xml}")])
            .arg(out.join(format!("{name}-protocol.h")))
            .status()
            .expect("wayland-scanner");
        assert!(status.success());
    }

    // The keymap, compiled at image build time, as a C string.
    let keymap = fs::read(format!("{prefix}/share/gm-keymap.xkb")).expect("keymap");
    let mut h = String::from("static const char gm_keymap[] = {");
    for b in keymap {
        h += &format!("{b},");
    }
    h += "0};\n";
    fs::write(out.join("keymap.h"), h).unwrap();

    let mut build = cc::Build::new();
    build
        .std("gnu11")
        .include(&out)
        .define("WLR_USE_UNSTABLE", None)
        .flag("-Wno-unused-parameter")
        .flag("-Wno-missing-field-initializers")
        .cargo_metadata(false);
    let mut all = STATIC_PKGS.to_vec();
    all.extend_from_slice(DYNAMIC_PKGS);
    for flag in pkg_config(&["--cflags"], &all) {
        build.flag(&flag);
    }
    let mut sources: Vec<_> = fs::read_dir("core")
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|e| e == "c"))
        .collect();
    sources.sort();
    build.files(&sources).compile("gmcore");

    // Static archives in one group so their order does not matter; the
    // host libraries after it.
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-arg={}", out.join("libgmcore.a").display());
    for flag in pkg_config(&["--static", "--libs"], STATIC_PKGS) {
        // libffi (wayland's) comes from the distro, which ships both kinds.
        let flag = if flag == "-lffi" { "-l:libffi.a".to_owned() } else { flag };
        println!("cargo:rustc-link-arg={flag}");
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    for flag in pkg_config(&["--libs"], DYNAMIC_PKGS) {
        println!("cargo:rustc-link-arg={flag}");
    }
}
