//! Plain UDP for native clients (PROTOCOL.md, "Plain UDP"): no retransmission.
//! The control stream's bytes travel encrypted in numbered datagrams that the
//! receiver puts back in order (auth.rs); video and audio use the same plain
//! datagrams as WebTransport's datagram mode. A client is its address: after a
//! handshake its first control datagram (number 0) starts a session, and a
//! session with no control datagram for 10 s ends (clients ping every 500 ms).
//! Only a datagram sealed with a session's keys touches that session, so a
//! forged handshake can't end one.
//!
//! The mouse has datagrams of its own, so that a lost control datagram can't hold
//! it up, and nothing waits for them: a motion datagram says how far the mouse has
//! gone since the start of the session, so one that is lost is in the next, and a
//! position is simply the latest. The client resends control datagrams until they
//! are acknowledged.

use std::collections::{BTreeMap, HashMap};
use std::future::Future;
use std::net::{Ipv6Addr, SocketAddr};
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use tokio::net::UdpSocket;
use tokio::sync::mpsc;

use crate::auth::{self, HostKey, Keys, Kind};
use crate::core;

pub const DATAGRAM_CONTROL: u8 = 3;
const DATAGRAM_HANDSHAKE: u8 = 6;
const DATAGRAM_MOUSE: u8 = 7;
const DATAGRAM_ACK: u8 = 8;
const DATAGRAM_PYROWAVE_LOSS: u8 = 11;
const MOUSE_MOTION: u8 = 0;
const MOUSE_POSITION: u8 = 1;
/// Largest datagram payload we send: fits a 1500-byte Ethernet/Wi-Fi MTU
/// under IPv6 and UDP headers.
pub const MAX_DATAGRAM: usize = 1400;
const CONTROL_HEADER: usize = 5;
const IDLE: Duration = Duration::from_secs(10);
/// Control datagrams held waiting for a missing earlier one; past this the
/// missing one is lost and the session ends (the client reconnects).
const MAX_OUT_OF_ORDER: usize = 256;
/// Handshakes that haven't gone on to a session.
const MAX_HANDSHAKES: usize = 64;

struct Route {
    tx: mpsc::UnboundedSender<Vec<u8>>,
    next: u32,
    pending: BTreeMap<u32, Vec<u8>>,
    keys: Arc<Keys>,
    ack_seq: u32,
    /// The mouse counts once the session has authorized the client.
    mouse_on: Arc<AtomicBool>,
    mouse_next: u32,
    mouse_total: (f64, f64),
}

impl Route {
    /// The next control datagram wanted, to the client.
    fn ack(&mut self, socket: &UdpSocket, addr: SocketAddr) {
        let seq = self.ack_seq;
        self.ack_seq += 1;
        let mut d = vec![DATAGRAM_ACK];
        d.extend_from_slice(&seq.to_le_bytes());
        let sealed = self.keys.seal(Kind::Ack, &d, seq, &self.next.to_le_bytes());
        d.extend_from_slice(&sealed);
        let _ = socket.try_send_to(&d, addr);
    }

    /// A mouse datagram's payload, unless a later one has come.
    fn mouse(&mut self, seq: u32, m: &[u8]) {
        if seq < self.mouse_next || !self.mouse_on.load(Ordering::Relaxed) {
            return;
        }
        self.mouse_next = seq + 1;
        match (m.first(), m.len()) {
            (Some(&MOUSE_MOTION), 17) => {
                let x = f64::from_le_bytes(m[1..9].try_into().unwrap());
                let y = f64::from_le_bytes(m[9..17].try_into().unwrap());
                let (dx, dy) = (x - self.mouse_total.0, y - self.mouse_total.1);
                self.mouse_total = (x, y);
                if dx != 0.0 || dy != 0.0 {
                    core::pointer_motion(dx, dy);
                }
            }
            (Some(&MOUSE_POSITION), 9) => {
                let x = f32::from_le_bytes(m[1..5].try_into().unwrap());
                let y = f32::from_le_bytes(m[5..9].try_into().unwrap());
                core::pointer_position(x as f64, y as f64);
            }
            _ => {}
        }
    }

    /// A PyroWave loss report: `u32 frame_id, u16 count, u16 n`, then n (first,
    /// length) ranges of missing packets. Reports are repeated until the host's
    /// frames say they were taken in, so neither order nor loss matters.
    fn pyrowave_loss(&mut self, m: &[u8]) {
        if !self.mouse_on.load(Ordering::Relaxed) || m.len() < 8 {
            return;
        }
        let frame_id = u32::from_le_bytes(m[0..4].try_into().unwrap());
        let count = u16::from_le_bytes([m[4], m[5]]);
        let n = u16::from_le_bytes([m[6], m[7]]) as usize;
        let ranges: Vec<u16> = m[8..].chunks_exact(2).take(2 * n).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
        if ranges.len() == 2 * n {
            core::pyrowave_loss(frame_id, count, &ranges);
        }
    }

    /// Takes in a control datagram's payload; false if the session is over.
    fn deliver(&mut self, seq: u32, payload: Vec<u8>) -> bool {
        if seq == self.next {
            let _ = self.tx.send(payload);
            self.next += 1;
            while let Some(p) = self.pending.remove(&self.next) {
                let _ = self.tx.send(p);
                self.next += 1;
            }
        } else if seq > self.next {
            self.pending.insert(seq, payload);
            return self.pending.len() <= MAX_OUT_OF_ORDER;
        }
        true
    }
}

/// A handshake answered, kept to answer it again if the reply was lost.
struct Handshake {
    client: Vec<u8>,
    reply: Vec<u8>,
    keys: Arc<Keys>,
}

type Routes = Arc<Mutex<HashMap<SocketAddr, Route>>>;

/// The host's side of one client.
pub struct Peer {
    socket: Arc<UdpSocket>,
    pub addr: SocketAddr,
    seq: AtomicU32,
    routes: Routes,
    keys: Arc<Keys>,
    mouse_on: Arc<AtomicBool>,
}

impl Peer {
    /// Authorized: the mouse datagrams count from now on.
    pub fn accept_mouse(&self) {
        self.mouse_on.store(true, Ordering::Relaxed);
    }

    pub fn keys(&self) -> &Keys {
        &self.keys
    }

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
        for chunk in bytes.chunks(MAX_DATAGRAM - CONTROL_HEADER - auth::TAG_SIZE) {
            let seq = self.seq.fetch_add(1, Ordering::Relaxed);
            let mut d = Vec::with_capacity(CONTROL_HEADER + chunk.len() + auth::TAG_SIZE);
            d.push(DATAGRAM_CONTROL);
            d.extend_from_slice(&seq.to_le_bytes());
            let sealed = self.keys.seal(Kind::Control, &d, seq, chunk);
            d.extend_from_slice(&sealed);
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

pub async fn serve<F, Fut>(port: u16, host: HostKey, session: F) -> Result<()>
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
    let mut handshakes: HashMap<SocketAddr, Handshake> = HashMap::new();
    let mut buf = vec![0u8; 65536];
    loop {
        let (n, addr) = socket.recv_from(&mut buf).await?;
        if n >= auth::HANDSHAKE_SIZE && buf[0] == DATAGRAM_HANDSHAKE {
            let client = &buf[1..33];
            if !handshakes.get(&addr).is_some_and(|h| h.client == client) {
                let Ok((body, keys)) = auth::respond(&host, client) else { continue };
                if handshakes.len() >= MAX_HANDSHAKES {
                    handshakes.clear();
                }
                let reply = [&[DATAGRAM_HANDSHAKE], body.as_slice()].concat();
                handshakes.insert(addr, Handshake { client: client.to_vec(), reply, keys: Arc::new(keys) });
            }
            let _ = socket.try_send_to(&handshakes[&addr].reply, addr);
            continue;
        }
        if n < CONTROL_HEADER + auth::TAG_SIZE
            || (buf[0] != DATAGRAM_CONTROL && buf[0] != DATAGRAM_MOUSE && buf[0] != DATAGRAM_PYROWAVE_LOSS)
        {
            continue;
        }
        let seq = u32::from_le_bytes(buf[1..5].try_into().unwrap());
        let (header, sealed) = buf[..n].split_at(CONTROL_HEADER);
        let mut map = routes.lock().unwrap();
        if header[0] == DATAGRAM_PYROWAVE_LOSS {
            if let Some(route) = map.get_mut(&addr) {
                if let Some(m) = route.keys.open(Kind::Loss, header, seq, sealed) {
                    route.pyrowave_loss(&m);
                }
            }
            continue;
        }
        if header[0] == DATAGRAM_MOUSE {
            if let Some(route) = map.get_mut(&addr) {
                if let Some(m) = route.keys.open(Kind::Mouse, header, seq, sealed) {
                    route.mouse(seq, &m);
                }
            }
            continue;
        }
        if let Some(route) = map.get_mut(&addr) {
            if let Some(payload) = route.keys.open(Kind::Control, header, seq, sealed) {
                if !route.deliver(seq, payload) {
                    eprintln!("gmstream: {addr}: control datagram {} lost; ending the session", route.next);
                    map.remove(&addr);
                } else {
                    route.ack(&socket, addr);
                }
                continue;
            }
        }
        // Not the session's, if there is one: the start of the next.
        let Some(payload) = handshakes.get(&addr).filter(|_| seq == 0).and_then(|h| h.keys.open(Kind::Control, header, seq, sealed)) else {
            continue;
        };
        let keys = handshakes.remove(&addr).unwrap().keys;
        let (tx, rx) = mpsc::unbounded_channel();
        let mouse_on = Arc::new(AtomicBool::new(false));
        let mut route = Route {
            tx,
            next: 0,
            pending: BTreeMap::new(),
            keys: keys.clone(),
            ack_seq: 0,
            mouse_on: mouse_on.clone(),
            mouse_next: 0,
            mouse_total: (0.0, 0.0),
        };
        route.deliver(seq, payload);
        route.ack(&socket, addr);
        map.insert(addr, route);
        let peer = Arc::new(Peer { socket: socket.clone(), addr, seq: AtomicU32::new(0), routes: routes.clone(), keys, mouse_on });
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
}

/// The largest UDP payload the route to `addr` carries without IP
/// fragmentation: the outgoing interface's MTU (as the kernel's route has it)
/// minus the IP and UDP headers. ~64 KB over a Thunderbolt link.
pub fn route_max_datagram(addr: SocketAddr) -> Option<usize> {
    let (ip, v6) = match addr {
        SocketAddr::V4(a) => (SocketAddr::V4(a), false),
        SocketAddr::V6(a) => match a.ip().to_ipv4_mapped() {
            Some(v4) => (SocketAddr::new(v4.into(), a.port()), false),
            None => (SocketAddr::V6(a), true),
        },
    };
    let bind: SocketAddr = if v6 { "[::]:0".parse().unwrap() } else { "0.0.0.0:0".parse().unwrap() };
    let socket = std::net::UdpSocket::bind(bind).ok()?;
    socket.connect(ip).ok()?; // no packet: only picks the route
    let (level, name) = if v6 { (libc::IPPROTO_IPV6, libc::IPV6_MTU) } else { (libc::IPPROTO_IP, libc::IP_MTU) };
    let mut mtu: libc::c_int = 0;
    let mut len = std::mem::size_of::<libc::c_int>() as libc::socklen_t;
    let r = unsafe { libc::getsockopt(socket.as_raw_fd(), level, name, &mut mtu as *mut _ as *mut libc::c_void, &mut len) };
    let headers = if v6 { 48 } else { 28 };
    (r == 0 && mtu as usize > headers).then(|| (mtu as usize - headers).min(65507))
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
