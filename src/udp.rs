//! Plain UDP for native clients (PROTOCOL.md, "Plain UDP"): no encryption,
//! no retransmission. The control stream's bytes travel in numbered
//! datagrams that the receiver puts back in order; video and audio use the
//! same datagrams as WebTransport's datagram mode. A client is its address:
//! its first control datagram (number 0) starts a session, and a session
//! with no control datagram for 10 s ends (clients ping every 500 ms).

use std::collections::{BTreeMap, HashMap};
use std::future::Future;
use std::net::{Ipv6Addr, SocketAddr};
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use tokio::net::UdpSocket;
use tokio::sync::mpsc;

pub const DATAGRAM_CONTROL: u8 = 3;
/// Largest datagram payload we send: fits a 1500-byte Ethernet/Wi-Fi MTU
/// under IPv6 and UDP headers.
pub const MAX_DATAGRAM: usize = 1400;
const CONTROL_HEADER: usize = 5;
const IDLE: Duration = Duration::from_secs(10);
/// Control datagrams held waiting for a missing earlier one; past this the
/// missing one is lost and the session ends (the client reconnects).
const MAX_OUT_OF_ORDER: usize = 256;

struct Route {
    tx: mpsc::UnboundedSender<Vec<u8>>,
    next: u32,
    pending: BTreeMap<u32, Vec<u8>>,
}

type Routes = Arc<Mutex<HashMap<SocketAddr, Route>>>;

/// The host's side of one client.
pub struct Peer {
    socket: Arc<UdpSocket>,
    pub addr: SocketAddr,
    seq: AtomicU32,
    routes: Routes,
}

impl Peer {
    /// Sends now or drops the datagram if the socket buffer is full.
    pub fn try_send(&self, d: &[u8]) -> Result<()> {
        match self.socket.try_send_to(d, self.addr) {
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => Ok(()),
            r => Ok(r.map(|_| ())?),
        }
    }

    /// Waits for room in the socket buffer: a keyframe is hundreds of
    /// datagrams, more than the kernel buffers at once.
    pub async fn send(&self, d: &[u8]) -> Result<()> {
        self.socket.send_to(d, self.addr).await?;
        Ok(())
    }

    /// Sends many datagrams with as few system calls as the kernel allows
    /// (sendmmsg): a PyroWave frame is ~250 datagrams, and one sendto each
    /// costs about as long as the frame takes on Wi-Fi. Waits for buffer room.
    pub async fn send_batch(&self, ds: &[Vec<u8>]) -> Result<()> {
        let (name, name_len) = sockaddr(self.addr);
        let mut off = 0;
        while off < ds.len() {
            self.socket.writable().await?;
            let r = self.socket.try_io(tokio::io::Interest::WRITABLE, || {
                let batch = &ds[off..ds.len().min(off + 64)];
                let mut iov: Vec<libc::iovec> = batch
                    .iter()
                    .map(|d| libc::iovec { iov_base: d.as_ptr() as *mut libc::c_void, iov_len: d.len() })
                    .collect();
                let mut msgs: Vec<libc::mmsghdr> = iov
                    .iter_mut()
                    .map(|v| {
                        let mut h: libc::mmsghdr = unsafe { std::mem::zeroed() };
                        h.msg_hdr.msg_name = &name as *const _ as *mut libc::c_void;
                        h.msg_hdr.msg_namelen = name_len;
                        h.msg_hdr.msg_iov = v;
                        h.msg_hdr.msg_iovlen = 1;
                        h
                    })
                    .collect();
                let n = unsafe { libc::sendmmsg(self.socket.as_raw_fd(), msgs.as_mut_ptr(), msgs.len() as libc::c_uint, 0) };
                if n < 0 { Err(std::io::Error::last_os_error()) } else { Ok(n as usize) }
            });
            match r {
                Ok(n) => off += n,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(e) => return Err(e.into()),
            }
        }
        Ok(())
    }

    /// Writes bytes of the host → client control stream.
    pub async fn send_control(&self, bytes: &[u8]) -> Result<()> {
        for chunk in bytes.chunks(MAX_DATAGRAM - CONTROL_HEADER) {
            let mut d = Vec::with_capacity(CONTROL_HEADER + chunk.len());
            d.push(DATAGRAM_CONTROL);
            d.extend_from_slice(&self.seq.fetch_add(1, Ordering::Relaxed).to_le_bytes());
            d.extend_from_slice(chunk);
            self.send(&d).await?;
        }
        Ok(())
    }

    /// Ends the session: its control stream reads fail from now on.
    pub fn close(&self) {
        self.routes.lock().unwrap().remove(&self.addr);
    }
}

/// The client → host control stream, in order.
pub struct Control {
    rx: mpsc::UnboundedReceiver<Vec<u8>>,
    buf: Vec<u8>,
}

impl Control {
    pub async fn read_exact(&mut self, out: &mut [u8]) -> Result<()> {
        while self.buf.len() < out.len() {
            match tokio::time::timeout(IDLE, self.rx.recv()).await {
                Ok(Some(b)) => self.buf.extend_from_slice(&b),
                Ok(None) => bail!("closed"),
                Err(_) => bail!("nothing from the client for {} s", IDLE.as_secs()),
            }
        }
        out.copy_from_slice(&self.buf[..out.len()]);
        self.buf.drain(..out.len());
        Ok(())
    }
}

fn set_buffer(socket: &std::net::UdpSocket, option: libc::c_int, bytes: libc::c_int) {
    // The kernel caps this at net.core.{w,r}mem_max; best effort.
    unsafe {
        libc::setsockopt(
            socket.as_raw_fd(),
            libc::SOL_SOCKET,
            option,
            &bytes as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );
    }
}

pub async fn serve<F, Fut>(port: u16, session: F) -> Result<()>
where
    F: Fn(Arc<Peer>, Control) -> Fut,
    Fut: Future<Output = Result<()>> + Send + 'static,
{
    let socket = std::net::UdpSocket::bind((Ipv6Addr::UNSPECIFIED, port)).with_context(|| format!("binding UDP port {port}"))?;
    set_buffer(&socket, libc::SO_SNDBUF, 8 << 20);
    set_buffer(&socket, libc::SO_RCVBUF, 1 << 20);
    // DSCP AF41 (interactive video): routers and Wi-Fi access points map it to
    // their video queue (WMM AC_VI) on the way to the client.
    let tclass: libc::c_int = 34 << 2;
    unsafe {
        for (level, opt) in [(libc::IPPROTO_IPV6, libc::IPV6_TCLASS), (libc::IPPROTO_IP, libc::IP_TOS)] {
            libc::setsockopt(socket.as_raw_fd(), level, opt, &tclass as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t);
        }
    }
    socket.set_nonblocking(true)?;
    let socket = Arc::new(UdpSocket::from_std(socket)?);
    let routes: Routes = Arc::default();
    let mut buf = vec![0u8; 65536];
    loop {
        let (n, addr) = socket.recv_from(&mut buf).await?;
        if n < CONTROL_HEADER || buf[0] != DATAGRAM_CONTROL {
            continue;
        }
        let seq = u32::from_le_bytes(buf[1..5].try_into().unwrap());
        let payload = buf[CONTROL_HEADER..n].to_vec();
        let mut map = routes.lock().unwrap();
        if !map.contains_key(&addr) {
            if seq != 0 {
                continue; // from a session that already ended
            }
            let (tx, rx) = mpsc::unbounded_channel();
            map.insert(addr, Route { tx, next: 0, pending: BTreeMap::new() });
            let peer = Arc::new(Peer { socket: socket.clone(), addr, seq: AtomicU32::new(0), routes: routes.clone() });
            let fut = session(peer, Control { rx, buf: Vec::new() });
            let routes = routes.clone();
            tokio::spawn(async move {
                if let Err(e) = fut.await {
                    eprintln!("gmstream: session ended: {e:#}");
                }
                // Unless the same address has started a new session since.
                let mut map = routes.lock().unwrap();
                if map.get(&addr).is_some_and(|r| r.tx.is_closed()) {
                    map.remove(&addr);
                }
            });
        }
        let route = map.get_mut(&addr).unwrap();
        if seq == route.next {
            let _ = route.tx.send(payload);
            route.next += 1;
            while let Some(p) = route.pending.remove(&route.next) {
                let _ = route.tx.send(p);
                route.next += 1;
            }
        } else if seq > route.next {
            route.pending.insert(seq, payload);
            if route.pending.len() > MAX_OUT_OF_ORDER {
                eprintln!("gmstream: {addr}: control datagram {} lost; ending the session", route.next);
                map.remove(&addr);
            }
        }
    }
}

/// A socket address in the form sendmmsg takes (the socket is IPv6, so IPv4
/// peers are IPv4-mapped).
fn sockaddr(addr: SocketAddr) -> (libc::sockaddr_in6, libc::socklen_t) {
    let v6 = match addr {
        SocketAddr::V6(a) => a,
        SocketAddr::V4(a) => std::net::SocketAddrV6::new(a.ip().to_ipv6_mapped(), a.port(), 0, 0),
    };
    let mut s: libc::sockaddr_in6 = unsafe { std::mem::zeroed() };
    s.sin6_family = libc::AF_INET6 as libc::sa_family_t;
    s.sin6_port = v6.port().to_be();
    s.sin6_addr.s6_addr = v6.ip().octets();
    s.sin6_flowinfo = v6.flowinfo();
    s.sin6_scope_id = v6.scope_id();
    (s, std::mem::size_of::<libc::sockaddr_in6>() as libc::socklen_t)
}
