//! HTTPS server for the web client, and the state it needs on disk.
//!
//! WebCodecs and WebTransport need a secure context, so the page is served
//! over TLS with a long-lived self-signed certificate kept in the state
//! directory (trust it once on the client, or click through the warning).
//! The WebTransport endpoint uses its own short-lived certificate, which the
//! page pins by hash (`/config.json`).

use std::fs;
use std::net::IpAddr;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Context, Result};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio_rustls::TlsAcceptor;
use tokio_rustls::rustls;
use tokio_rustls::rustls::pki_types::pem::PemObject;
use tokio_rustls::rustls::pki_types::{CertificateDer, PrivateKeyDer};

use crate::session::CertHash;

const INDEX_HTML: &str = include_str!("../web/index.html");
const CLIENT_JS: &str = include_str!("../web/client.js");

pub fn state_dir() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/state"));
    base.join("gmstream")
}

/// The access token clients must present; created on first run.
pub fn load_token(dir: &Path) -> Result<String> {
    let path = dir.join("token");
    if let Ok(t) = fs::read_to_string(&path) {
        let t = t.trim().to_owned();
        if !t.is_empty() {
            return Ok(t);
        }
    }
    let mut bytes = [0u8; 16];
    std::io::Read::read_exact(&mut fs::File::open("/dev/urandom")?, &mut bytes)?;
    let t: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
    write_private(&path, t.as_bytes())?;
    Ok(t)
}

fn write_private(path: &Path, data: &[u8]) -> Result<()> {
    use std::os::unix::fs::OpenOptionsExt;
    let mut f = fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(path)?;
    std::io::Write::write_all(&mut f, data)?;
    Ok(())
}

pub fn hostname() -> String {
    let mut buf = [0u8; 256];
    unsafe { libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) };
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..end]).into_owned()
}

fn local_addresses() -> Vec<IpAddr> {
    let mut out = Vec::new();
    unsafe {
        let mut ifap: *mut libc::ifaddrs = std::ptr::null_mut();
        if libc::getifaddrs(&mut ifap) != 0 {
            return out;
        }
        let mut p = ifap;
        while !p.is_null() {
            let a = (*p).ifa_addr;
            if !a.is_null() {
                match (*a).sa_family as i32 {
                    libc::AF_INET => {
                        let sin = &*(a as *const libc::sockaddr_in);
                        out.push(IpAddr::from(u32::from_be(sin.sin_addr.s_addr).to_be_bytes()));
                    }
                    libc::AF_INET6 => {
                        let sin6 = &*(a as *const libc::sockaddr_in6);
                        let ip = std::net::Ipv6Addr::from(sin6.sin6_addr.s6_addr);
                        if ip.segments()[0] & 0xffc0 != 0xfe80 {
                            out.push(IpAddr::V6(ip));
                        }
                    }
                    _ => {}
                }
            }
            p = (*p).ifa_next;
        }
        libc::freeifaddrs(ifap);
    }
    out.sort();
    out.dedup();
    out
}

/// Loads the page certificate, (re)creating it when the host's names or
/// addresses changed.
fn page_tls(dir: &Path) -> Result<rustls::ServerConfig> {
    let host = hostname();
    let mut names = vec![host.clone(), format!("{host}.local"), "localhost".to_owned()];
    names.extend(local_addresses().iter().map(|a| a.to_string()));
    let names_txt = names.join("\n");
    let (cert_path, key_path, names_path) = (dir.join("cert.pem"), dir.join("key.pem"), dir.join("cert.names"));

    if fs::read_to_string(&names_path).ok().as_deref() != Some(names_txt.as_str())
        || !cert_path.exists()
        || !key_path.exists()
    {
        let key = rcgen::KeyPair::generate()?;
        let mut params = rcgen::CertificateParams::new(names.clone())?;
        params.distinguished_name.push(rcgen::DnType::CommonName, format!("gmstream on {host}"));
        params.extended_key_usages = vec![rcgen::ExtendedKeyUsagePurpose::ServerAuth];
        // macOS rejects TLS certificates valid for more than 825 days.
        let now = std::time::SystemTime::now();
        params.not_before = (now - std::time::Duration::from_secs(86400)).into();
        params.not_after = (now + std::time::Duration::from_secs(800 * 86400)).into();
        let cert = params.self_signed(&key)?;
        fs::write(&cert_path, cert.pem())?;
        write_private(&key_path, key.serialize_pem().as_bytes())?;
        fs::write(&names_path, &names_txt)?;
        eprintln!("gmstream: new page certificate for {}", names.join(", "));
    }

    let certs = CertificateDer::pem_file_iter(&cert_path)?.collect::<Result<Vec<_>, _>>()?;
    let key = PrivateKeyDer::from_pem_file(&key_path)?;
    let mut config = rustls::ServerConfig::builder().with_no_client_auth().with_single_cert(certs, key)?;
    config.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(config)
}

pub async fn serve(port: u16, dir: PathBuf, hash: CertHash) -> Result<()> {
    let acceptor = TlsAcceptor::from(Arc::new(page_tls(&dir)?));
    let listener = TcpListener::bind(("::", port)).await.with_context(|| format!("binding TCP port {port}"))?;
    loop {
        let (tcp, _) = listener.accept().await?;
        let _ = tcp.set_nodelay(true);
        let acceptor = acceptor.clone();
        let hash = hash.clone();
        tokio::spawn(async move {
            let Ok(mut tls) = acceptor.accept(tcp).await else { return };
            let mut req = Vec::new();
            let mut buf = [0u8; 4096];
            while !req.windows(4).any(|w| w == b"\r\n\r\n") && req.len() < 16384 {
                match tls.read(&mut buf).await {
                    Ok(0) | Err(_) => return,
                    Ok(n) => req.extend_from_slice(&buf[..n]),
                }
            }
            let line = String::from_utf8_lossy(&req);
            let path = line.split_whitespace().nth(1).unwrap_or("/");
            let path = path.split(['?', '#']).next().unwrap_or("/");
            let (status, ty, body) = match path {
                "/" | "/index.html" => ("200 OK", "text/html; charset=utf-8", INDEX_HTML.to_owned()),
                "/client.js" => ("200 OK", "text/javascript; charset=utf-8", CLIENT_JS.to_owned()),
                "/config.json" => (
                    "200 OK",
                    "application/json",
                    format!("{{\"port\":{port},\"certHash\":{}}}", hash.read().unwrap()),
                ),
                _ => ("404 Not Found", "text/plain", "not found\n".to_owned()),
            };
            let head = format!(
                "HTTP/1.1 {status}\r\nContent-Type: {ty}\r\nContent-Length: {}\r\n\
                 Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                body.len()
            );
            let _ = tls.write_all(head.as_bytes()).await;
            let _ = tls.write_all(body.as_bytes()).await;
            let _ = tls.shutdown().await;
        });
    }
}
