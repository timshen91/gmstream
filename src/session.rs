//! WebTransport server and the client protocol (see PROTOCOL.md).
//!
//! One client streams at a time; a new one replaces the old. Video frames go
//! out each on their own unidirectional stream (newer frames at higher
//! priority, so a frame stuck on loss never delays the next), all on one
//! stream, or split into datagrams, as the client asks in Hello. Audio goes
//! as datagrams, everything else on the client's bidirectional control stream.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use tokio::sync::mpsc;
use wtransport::endpoint::IncomingSession;
use wtransport::quinn;
use wtransport::quinn::congestion::CubicConfig;
use wtransport::tls::Sha256DigestFmt;
use wtransport::{Endpoint, Identity, ServerConfig};

use crate::auth::{self, HostKey};
use crate::core;
use crate::transport::{Conn, RecvStream, SendStream};
use crate::udp;

// Version 2 added the Hello flags byte, version 3 the client's largest
// datagram, version 4 the handshake and Auth over plain UDP, version 5 the mouse
// datagrams; older web clients are still served.
pub const PROTOCOL_VERSION: u16 = 5;

// A PyroWave datagram's header (send_pyrowave), and a Riptide one
// (Sink::riptide).
const PYROWAVE_HEADER: usize = 29;
const RIPTIDE_HEADER: usize = 43;

// Hello flags.
const HELLO_ONE_VIDEO_STREAM: u8 = 1;
const HELLO_VIDEO_DATAGRAMS: u8 = 2;
const HELLO_VIDEO_CHUNKS: u8 = 4;
const HELLO_HDR: u8 = 8;
/// The client asks for every unit to end on one of its Wi-Fi receive flushes
/// (PROTOCOL.md, "Flush alignment"); plain UDP only.
const HELLO_FLUSH_ALIGN: u8 = 32;

// Client → host control messages.
const C_HELLO: u8 = 0x01;
const C_CONFIGURE: u8 = 0x02;
const C_KEYFRAME: u8 = 0x03;
const C_BITRATE: u8 = 0x04;
const C_KEY: u8 = 0x05;
const C_BUTTON: u8 = 0x06;
const C_MOTION: u8 = 0x07;
const C_POSITION: u8 = 0x08;
const C_SCROLL: u8 = 0x09;
const C_GAMEPAD: u8 = 0x0a;
const C_GAMEPAD_REMOVE: u8 = 0x0b;
const C_PING: u8 = 0x0c;
const C_DISPLAY_GRID: u8 = 0x0d;
const C_VRR_POLICY: u8 = 0x0e;
const C_TOUCH: u8 = 0x0f;
const C_AUTH: u8 = 0x10;
const C_PARALLELISM: u8 = 0x11;
const C_ROTATION: u8 = 0x12;
// Host → client.
const H_HELLO_ACK: u8 = 0x81;
const H_CURSOR: u8 = 0x82;
const H_RELATIVE: u8 = 0x83;
const H_CURSOR_CLIP: u8 = 0x87;
const H_ENCODE_FORMAT: u8 = 0x88;
const H_RUMBLE: u8 = 0x84;
const H_PONG: u8 = 0x85;
const H_ERROR: u8 = 0x86;

const DATAGRAM_AUDIO: u8 = 1;
const DATAGRAM_VIDEO: u8 = 2;
const VIDEO_FRAGMENT_HEADER: usize = 9;
const DATAGRAM_PYROWAVE: u8 = 4;
const DATAGRAM_VIDEO_CHUNK: u8 = 9;
const DATAGRAM_RIPTIDE: u8 = 10;
/// One byte, host -> client, dropped by the client: makes up a unit's count
/// (FlushAlign).
const DATAGRAM_FILL: u8 = 12;
const VIDEO_CHUNK_HEADER: usize = 43;
const CODEC_HEVC: u8 = 1;
const CODEC_AV1: u8 = 2;
const CODEC_PYROWAVE: u8 = 3;
const CODEC_RIPTIDE: u8 = 4;
const VIDEO_HEADER: usize = 32;
// A frame that has not reached the client by then is abandoned; the client
// asks for a keyframe instead.
const FRAME_DEADLINE: Duration = Duration::from_millis(250);

fn message(ty: u8, payload: &[u8]) -> Vec<u8> {
    let mut m = Vec::with_capacity(5 + payload.len());
    m.push(ty);
    m.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    m.extend_from_slice(payload);
    m
}

/// Pads each unit the host sends (a Riptide stripe, a frame in pieces, a
/// whole frame) with DATAGRAM_FILL so that its datagrams come to a multiple of
/// FLUSH_PACKETS, for clients that ask (PROTOCOL.md, "Flush alignment").
/// macOS's Wi-Fi driver hands a burst up to the socket as soon as 32 packets
/// are pending, and otherwise only on a timer ~1.4 ms into the burst; a unit
/// that ends on a multiple of 32 is handed up when its last datagram lands.
/// Up to three datagrams after quiet are handed up at once and do not count,
/// so a unit that small after quiet is left as it is. Measured in DESIGN.md §14.2.
#[derive(Default)]
struct FlushAlign {
    /// Datagrams sent since the last unit ended on a flush.
    pending: usize,
    last_ns: u64,
}

const FLUSH_PACKETS: usize = 32;
/// A gap after which the client's receive path has gone back to delivering at
/// once (it does after ~1 ms; the margin keeps a near miss from misaligning).
const FLUSH_QUIET_NS: u64 = 2_000_000;

impl FlushAlign {
    /// `n` more datagrams of a unit are going out; at the unit's end, returns
    /// how many fillers to send after them.
    fn add(&mut self, n: usize, end: bool) -> usize {
        let now = core::now_ns();
        let quiet = now.saturating_sub(self.last_ns) > FLUSH_QUIET_NS;
        self.last_ns = now;
        if quiet {
            self.pending = 0;
            if end && n <= 3 {
                return 0;
            }
        }
        self.pending += n;
        if !end {
            return 0;
        }
        let fill = (FLUSH_PACKETS - self.pending % FLUSH_PACKETS) % FLUSH_PACKETS;
        self.pending = 0;
        fill
    }
}

fn fillers(n: usize) -> impl Iterator<Item = Vec<u8>> {
    (0..n).map(|_| vec![DATAGRAM_FILL])
}

struct Client {
    conn: Arc<Conn>,
    /// Some when the client asked for flush alignment.
    flush: Option<Arc<Mutex<FlushAlign>>>,
    video: mpsc::Sender<(u32, Vec<u8>)>,
    /// Pieces of frames, ready to send; a frame is ~150 of them.
    chunks: mpsc::Sender<Vec<u8>>,
    ctrl: mpsc::UnboundedSender<Vec<u8>>,
    audio_seq: AtomicU32,
    stats: Arc<VideoStats>,
}

/// Where frames go missing, logged when anything does.
#[derive(Default)]
struct VideoStats {
    encoded: AtomicU64,
    queue_full: AtomicU64,
    open_timeout: AtomicU64,
    write_timeout: AtomicU64,
    sent: AtomicU64,
}

impl VideoStats {
    fn take(&self) -> [u64; 5] {
        [&self.encoded, &self.queue_full, &self.open_timeout, &self.write_timeout, &self.sent]
            .map(|c| c.swap(0, Ordering::Relaxed))
    }
}

/// Routes the core's callbacks to the current client.
pub struct Hub {
    current: Mutex<Option<Arc<Client>>>,
    sessions: AtomicU64,
}

pub static HUB: Hub = Hub { current: Mutex::new(None), sessions: AtomicU64::new(0) };

impl Hub {
    fn client(&self) -> Option<Arc<Client>> {
        self.current.lock().unwrap().clone()
    }
    fn replace(&self, client: Arc<Client>) -> Option<Arc<Client>> {
        self.current.lock().unwrap().replace(client)
    }
    /// Clears the current client if it is still `client`.
    fn remove(&self, client: &Arc<Client>) -> bool {
        let mut cur = self.current.lock().unwrap();
        if cur.as_ref().is_some_and(|c| Arc::ptr_eq(c, client)) {
            *cur = None;
            true
        } else {
            false
        }
    }
    fn control(&self, msg: Vec<u8>) {
        if let Some(c) = self.client() {
            let _ = c.ctrl.send(msg);
        }
    }
}

impl core::Sink for Hub {
    fn video(&self, p: &core::VideoPacket) {
        let Some(c) = self.client() else { return };
        let data = p.data();
        let mut buf = Vec::with_capacity(VIDEO_HEADER + data.len());
        buf.extend_from_slice(&p.frame_id.to_le_bytes());
        buf.push(p.keyframe as u8);
        buf.push(p.codec as u8);
        buf.extend_from_slice(&p.width.to_le_bytes());
        buf.extend_from_slice(&p.height.to_le_bytes());
        buf.extend_from_slice(&[0, 0]);
        buf.extend_from_slice(&p.slot_ns.to_le_bytes());
        buf.extend_from_slice(&p.encoded_ns.to_le_bytes());
        buf.extend_from_slice(&[0, 0, 0, 0]);
        buf.extend_from_slice(data);
        c.stats.encoded.fetch_add(1, Ordering::Relaxed);
        if c.video.try_send((p.frame_id, buf)).is_err() {
            c.stats.queue_full.fetch_add(1, Ordering::Relaxed);
            // The network side is behind; the client will see a gap.
            core::request_keyframe();
        }
    }

    /// One piece of a frame, as the encoder writes it (PROTOCOL.md, type 9).
    /// Called on the encoder thread, so it only queues: the chunk task sends.
    fn video_chunk(&self, c: &core::VideoChunk) {
        let Some(client) = self.client() else { return };
        let data = c.data();
        let mut d = Vec::with_capacity(VIDEO_CHUNK_HEADER + data.len());
        d.push(DATAGRAM_VIDEO_CHUNK);
        d.extend_from_slice(&c.frame_id.to_le_bytes());
        d.extend_from_slice(&c.index.to_le_bytes());
        d.extend_from_slice(&c.count.to_le_bytes());
        d.extend_from_slice(&c.offset.to_le_bytes());
        d.extend_from_slice(&c.total.to_le_bytes());
        d.push(c.keyframe as u8 | (c.last as u8) << 1);
        d.push(c.codec as u8);
        d.extend_from_slice(&c.width.to_le_bytes());
        d.extend_from_slice(&c.height.to_le_bytes());
        d.extend_from_slice(&c.slot_ns.to_le_bytes());
        d.extend_from_slice(&c.encoded_ns.to_le_bytes());
        // How much of the sending happened before the bitstream was finished:
        // the client takes the network's share from the first piece, not from
        // the end of the encode.
        let overlap = c.encoded_ns.saturating_sub(c.first_ns).min(u32::MAX as u64) as u32;
        d.extend_from_slice(&(if c.first_ns > 0 { overlap } else { 0 }).to_le_bytes());
        d.extend_from_slice(data);
        if c.last {
            client.stats.encoded.fetch_add(1, Ordering::Relaxed);
        }
        if client.chunks.try_send(d).is_err() {
            // The network side is behind: this frame is incomplete, so the
            // client needs a fresh one to decode from.
            client.stats.queue_full.fetch_add(1, Ordering::Relaxed);
            core::request_keyframe();
        }
        // The frame is the unit: it ends on a flush.
        if let Some(f) = &client.flush {
            for d in fillers(f.lock().unwrap().add(1, c.last)) {
                let _ = client.chunks.try_send(d);
            }
        }
    }

    /// A Riptide stripe's packets, one datagram each, out
    /// as soon as they are queued (an inter frame's inter packets before the rest
    /// of the frame is even coded).
    fn riptide(&self, p: &core::RiptidePackets) {
        let Some(client) = self.client() else { return };
        let mut rest = p.data();
        let mut index = p.first;
        while rest.len() >= 2 {
            let len = u16::from_le_bytes([rest[0], rest[1]]) as usize;
            let Some(packet) = rest.get(2..2 + len) else { break };
            let mut d = Vec::with_capacity(RIPTIDE_HEADER + len);
            d.push(DATAGRAM_RIPTIDE);
            d.extend_from_slice(&p.frame_id.to_le_bytes());
            d.extend_from_slice(&index.to_le_bytes());
            d.extend_from_slice(&p.width.to_le_bytes());
            d.extend_from_slice(&p.height.to_le_bytes());
            d.extend_from_slice(&p.slot_ns.to_le_bytes());
            d.extend_from_slice(&p.encoded_ns.to_le_bytes());
            d.push(p.predicted as u8);
            d.extend_from_slice(&p.repaired.to_le_bytes());
            d.push(p.stripe);
            d.push(p.stripes);
            d.extend_from_slice(&p.first_rows.to_le_bytes());
            d.extend_from_slice(&p.stripe_rows.to_le_bytes());
            d.extend_from_slice(&p.first.to_le_bytes());
            d.extend_from_slice(&p.count.to_le_bytes());
            d.push(p.inter);
            d.extend_from_slice(packet);
            if client.chunks.try_send(d).is_err() {
                // The network side is behind: the client misses these and says so.
                client.stats.queue_full.fetch_add(1, Ordering::Relaxed);
            }
            index += 1;
            rest = &rest[2 + len..];
        }
        // The stripe is the unit: it ends on a flush.
        if let Some(f) = &client.flush {
            for d in fillers(f.lock().unwrap().add((index - p.first) as usize, true)) {
                let _ = client.chunks.try_send(d);
            }
        }
        if p.stripe + 1 == p.stripes {
            client.stats.encoded.fetch_add(1, Ordering::Relaxed);
            client.stats.sent.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn audio(&self, opus: &[u8], pts_ns: u64) {
        let Some(c) = self.client() else { return };
        let mut d = Vec::with_capacity(13 + opus.len());
        d.push(DATAGRAM_AUDIO);
        d.extend_from_slice(&c.audio_seq.fetch_add(1, Ordering::Relaxed).to_le_bytes());
        d.extend_from_slice(&pts_ns.to_le_bytes());
        d.extend_from_slice(opus);
        let _ = c.conn.send_datagram(d);
    }

    fn cursor(&self, cursor: &core::Cursor) {
        let mut p = Vec::new();
        let (w, h) = if cursor.rgba().is_some() { (cursor.width, cursor.height) } else { (0, 0) };
        p.extend_from_slice(&w.to_le_bytes());
        p.extend_from_slice(&h.to_le_bytes());
        p.extend_from_slice(&cursor.hotspot_x.to_le_bytes());
        p.extend_from_slice(&cursor.hotspot_y.to_le_bytes());
        if let Some(rgba) = cursor.rgba() {
            p.extend_from_slice(rgba);
        }
        self.control(message(H_CURSOR, &p));
    }

    fn relative(&self, relative: bool) {
        self.control(message(H_RELATIVE, &[relative as u8]));
    }

    fn cursor_clip(&self, clip: [u16; 4]) {
        let p: Vec<u8> = clip.iter().flat_map(|v| v.to_le_bytes()).collect();
        self.control(message(H_CURSOR_CLIP, &p));
    }

    fn rumble(&self, pad: u8, strong: u16, weak: u16, duration_ms: u16) {
        let mut p = vec![pad];
        p.extend_from_slice(&strong.to_le_bytes());
        p.extend_from_slice(&weak.to_le_bytes());
        p.extend_from_slice(&duration_ms.to_le_bytes());
        self.control(message(H_RUMBLE, &p));
    }

    fn encode_format(&self, text: &str) {
        eprintln!("gmstream: encoding from {text}");
        self.control(message(H_ENCODE_FORMAT, text.as_bytes()));
    }
}

/// The current WebTransport certificate's SHA-256, as a JSON byte array.
pub type CertHash = Arc<RwLock<String>>;

fn new_identity() -> Result<Identity> {
    // serverCertificateHashes only accepts certificates valid for <= 14 days.
    Ok(Identity::self_signed_builder()
        .subject_alt_names(["localhost"])
        .from_now_utc()
        .validity_days(13)
        .build()?)
}

fn transport_config() -> quinn::TransportConfig {
    let mut transport = quinn::TransportConfig::default();
    // LAN: start with a window that fits a large keyframe instead of probing
    // up to it over several round trips.
    let mut cubic = CubicConfig::default();
    cubic.initial_window(4 << 20);
    transport.congestion_controller_factory(Arc::new(cubic));
    transport.send_window(16 << 20);
    // Datagram video sends each keyframe as one burst (over 1 MB at high
    // bitrates); when this buffer is full, quinn drops the oldest datagrams.
    transport.datagram_send_buffer_size(16 << 20);
    transport.keep_alive_interval(Some(Duration::from_secs(2)));
    transport.max_idle_timeout(Some(Duration::from_secs(10).try_into().expect("idle timeout")));
    transport
}

fn server_config(port: u16, identity: Identity) -> ServerConfig {
    ServerConfig::builder()
        .with_bind_default(port)
        .with_custom_transport(identity, transport_config())
        .keep_alive_interval(Some(Duration::from_secs(2)))
        .max_idle_timeout(Some(Duration::from_secs(10)))
        .expect("idle timeout")
        .build()
}

/// Native clients, on plain UDP.
pub async fn serve_udp(port: u16, host: HostKey) -> Result<()> {
    udp::serve(port, host, |peer, control| {
        session(Arc::new(Conn::Udp(peer.clone())), SendStream::Udp(peer), RecvStream::Udp(control))
    })
    .await
}

/// The web client (--web). Anyone who reaches it can connect: it has no key.
pub async fn serve_web(port: u16, hash: CertHash) -> Result<()> {
    let identity = new_identity()?;
    *hash.write().unwrap() = identity.certificate_chain().as_slice()[0]
        .hash()
        .fmt(Sha256DigestFmt::BytesArray);
    let endpoint = Arc::new(Endpoint::server(server_config(port, identity))?);

    // Rotate the certificate well before it expires.
    let ep = endpoint.clone();
    tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(7 * 24 * 3600)).await;
            match new_identity() {
                Ok(identity) => {
                    let h = identity.certificate_chain().as_slice()[0]
                        .hash()
                        .fmt(Sha256DigestFmt::BytesArray);
                    if ep.reload_config(server_config(port, identity), false).is_ok() {
                        *hash.write().unwrap() = h;
                    }
                }
                Err(e) => eprintln!("gmstream: certificate rotation failed: {e}"),
            }
        }
    });

    loop {
        let incoming = endpoint.accept().await;
        tokio::spawn(async move {
            if let Err(e) = handle_wt(incoming).await {
                eprintln!("gmstream: session ended: {e:#}");
            }
        });
    }
}

async fn read_message(recv: &mut RecvStream) -> Result<(u8, Vec<u8>)> {
    let mut head = [0u8; 5];
    recv.read_exact(&mut head).await.context("control stream closed")?;
    let len = u32::from_le_bytes(head[1..5].try_into().unwrap()) as usize;
    if len > 1 << 20 {
        bail!("control message too large");
    }
    let mut body = vec![0u8; len];
    recv.read_exact(&mut body).await.context("control stream closed")?;
    Ok((head[0], body))
}

struct Reader<'a>(&'a [u8]);
impl Reader<'_> {
    fn take<const N: usize>(&mut self) -> Result<[u8; N]> {
        if self.0.len() < N {
            bail!("short message");
        }
        let (a, b) = self.0.split_at(N);
        self.0 = b;
        Ok(a.try_into().unwrap())
    }
    fn u8(&mut self) -> Result<u8> {
        Ok(self.take::<1>()?[0])
    }
    fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_le_bytes(self.take()?))
    }
    fn i16(&mut self) -> Result<i16> {
        Ok(i16::from_le_bytes(self.take()?))
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.take()?))
    }
    fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.take()?))
    }
    fn f32(&mut self) -> Result<f64> {
        Ok(f32::from_le_bytes(self.take()?) as f64)
    }
}

async fn send_error(send: &mut SendStream, text: &str) {
    let _ = send.write_all(&message(H_ERROR, text.as_bytes())).await;
    let _ = send.finish().await;
}

/// A PyroWave frame (frame header, then its packets each after a u16 length)
/// as one datagram per packet, each carrying the frame's id, size and times:
/// packets decode on their own, so a lost one only blurs part of the picture.
/// Returns false if the transport failed.
/// Measuring (GM_DEBUG_SEND): per PyroWave frame, encode done → send
/// starts and send starts → last datagram handed to the kernel, ns.
static SEND_TIMES: Mutex<Vec<(u64, u64)>> = Mutex::new(Vec::new());

fn send_times_summary() -> Option<String> {
    let mut v = std::mem::take(&mut *SEND_TIMES.lock().unwrap());
    if v.is_empty() {
        return None;
    }
    let pct = |v: &mut Vec<(u64, u64)>, f: fn(&(u64, u64)) -> u64| {
        v.sort_by_key(f);
        let at = |q: f64| f(&v[((v.len() - 1) as f64 * q) as usize]) as f64 / 1e6;
        format!("p50 {:.2} p90 {:.2} max {:.2} ms", at(0.5), at(0.9), at(1.0))
    };
    Some(format!("encoded→send {}, send {}", pct(&mut v, |t| t.0), pct(&mut v, |t| t.1)))
}

async fn send_pyrowave(conn: &Conn, buf: &[u8], flush: Option<&Mutex<FlushAlign>>) -> bool {
    let start = core::now_ns();
    let (head, mut rest) = buf.split_at(VIDEO_HEADER);
    let mut packets = Vec::new();
    while rest.len() >= 2 {
        let len = u16::from_le_bytes([rest[0], rest[1]]) as usize;
        let Some(p) = rest.get(2..2 + len) else { break };
        packets.push(p);
        rest = &rest[2 + len..];
    }
    let count = packets.len() as u16;
    let mut ds = Vec::with_capacity(packets.len());
    for (i, p) in packets.into_iter().enumerate() {
        let mut d = Vec::with_capacity(PYROWAVE_HEADER + p.len());
        d.push(DATAGRAM_PYROWAVE);
        d.extend_from_slice(&head[0..4]); // frame id
        d.extend_from_slice(&(i as u16).to_le_bytes());
        d.extend_from_slice(&count.to_le_bytes());
        d.extend_from_slice(&head[6..10]); // width, height
        d.extend_from_slice(&head[12..28]); // slot_ns, encoded_ns
        d.extend_from_slice(p);
        ds.push(d);
    }
    if let Some(f) = flush {
        let n = f.lock().unwrap().add(ds.len(), true);
        ds.extend(fillers(n));
    }
    let ok = conn.send_datagrams_wait(ds).await.is_ok();
    if std::env::var_os("GM_DEBUG_SEND").is_some() {
        let encoded = u64::from_le_bytes(head[20..28].try_into().unwrap());
        SEND_TIMES.lock().unwrap().push((start.saturating_sub(encoded), core::now_ns() - start));
    }
    ok
}

async fn send_frame(conn: Arc<Conn>, stats: Arc<VideoStats>, id: u32, buf: Vec<u8>) {
    // Opening waits for the client to grant stream credit.
    let Ok(Ok(mut stream)) = tokio::time::timeout(FRAME_DEADLINE, conn.open_uni()).await else {
        stats.open_timeout.fetch_add(1, Ordering::Relaxed);
        return;
    };
    stream.set_priority(id as i32);
    let sent = tokio::time::timeout(FRAME_DEADLINE, async {
        stream.write_all(&buf).await?;
        stream.finish().await
    })
    .await;
    match sent {
        Ok(Ok(_)) => stats.sent.fetch_add(1, Ordering::Relaxed),
        _ => stats.write_timeout.fetch_add(1, Ordering::Relaxed),
    };
    if sent.is_err() {
        stream.reset(1);
    }
}

async fn handle_wt(incoming: IncomingSession) -> Result<()> {
    let conn = incoming.await?.accept().await?;
    let (send, recv) = conn.accept_bi().await?;
    session(Arc::new(Conn::Wt(conn)), SendStream::Wt(send), RecvStream::Wt(recv)).await
}

async fn session(conn: Arc<Conn>, mut send: SendStream, mut recv: RecvStream) -> Result<()> {
    let peer = conn.remote_address();

    let (mut ty, mut body) = read_message(&mut recv).await?;
    if let Conn::Udp(udp) = &*conn {
        if ty != C_AUTH {
            bail!("{peer}: expected Auth");
        }
        if let Err(e) = auth::verify(udp.keys(), &body) {
            send_error(&mut send, "This device isn't authorized: add its key, from Settings, to ~/.ssh/authorized_keys on the host").await;
            bail!("{peer}: {e:#}");
        }
        (ty, body) = read_message(&mut recv).await?;
    }
    let mut r = Reader(&body);
    if ty != C_HELLO {
        bail!("{peer}: expected Hello");
    }
    let version = r.u16()?;
    let (width, height, refresh, kbps) = (r.u16()?, r.u16()?, r.u32()?, r.u32()?);
    let (mask, pref) = (r.u8()?, r.u8()?);
    let flags = if version >= 2 { r.u8()? } else { 0 };
    let client_max_datagram = if version >= 3 { r.u16()? as usize } else { 0 };
    let datagrams = conn.is_udp() || flags & HELLO_VIDEO_DATAGRAMS != 0;
    let wants_chunks = flags & HELLO_VIDEO_CHUNKS != 0;
    let wants_hdr = flags & HELLO_HDR != 0;
    let wants_flush = conn.is_udp() && flags & HELLO_FLUSH_ALIGN != 0;
    let one_stream = !datagrams && flags & HELLO_ONE_VIDEO_STREAM != 0;
    if !(1..=PROTOCOL_VERSION).contains(&version) {
        send_error(&mut send, "Protocol version mismatch; reload the page").await;
        bail!("{peer}: protocol version {version}");
    }
    // Riptide only over plain UDP, where the client reports its losses.
    let host = core::supported_codecs() & if conn.is_udp() { !0 } else { !(1 << CODEC_RIPTIDE) };
    let both = mask as u32 & host;
    let codec = if both & (1 << pref) != 0 {
        pref
    } else if let Some(c) = (0..5).find(|c| both & (1 << c) != 0) {
        c
    } else {
        send_error(&mut send, "No video codec in common with this host").await;
        bail!("{peer}: no common codec (client {mask:#x}, host {:#x})", core::supported_codecs());
    };
    let kbps = kbps.clamp(1000, 500_000);
    // A frame in pieces as the encoder writes it (core/vkencode.c): the two
    // codecs the Vulkan encoder takes, and only for a client that asked, since
    // the pieces are a different datagram. The host falls back to whole frames
    // on its own if that encoder is not the one running.
    let chunked = wants_chunks && (codec == CODEC_AV1 || codec == CODEC_HEVC);
    // The largest datagram this link carries unfragmented, so a piece of a
    // frame or a PyroWave packet fills one (each datagram has a fixed cost;
    // see core/pyrowave.c). 0: the core's default.
    let link_max = match (conn.is_udp(), udp::route_max_datagram(peer)) {
        (true, Some(host_max)) if client_max_datagram > 0 => Some(client_max_datagram.min(host_max)),
        (false, _) => conn.max_datagram_size(),
        _ => None,
    };
    let packet = match (chunked, link_max) {
        (true, Some(max)) => max.saturating_sub(VIDEO_CHUNK_HEADER),
        (false, Some(max)) if codec == CODEC_PYROWAVE && conn.is_udp() => max.saturating_sub(PYROWAVE_HEADER),
        (false, Some(max)) if codec == CODEC_RIPTIDE => max.saturating_sub(RIPTIDE_HEADER),
        _ => 0,
    };

    let (vtx, mut vrx) = mpsc::channel::<(u32, Vec<u8>)>(4);
    // Deep enough for a keyframe, which is a few thousand pieces.
    let (chtx, mut chrx) = mpsc::channel::<Vec<u8>>(8192);
    let (ctx, mut crx) = mpsc::unbounded_channel::<Vec<u8>>();
    let flush = wants_flush.then(|| Arc::new(Mutex::new(FlushAlign::default())));
    let client = Arc::new(Client { conn: conn.clone(), flush: flush.clone(), video: vtx, chunks: chtx, ctrl: ctx, audio_seq: AtomicU32::new(0), stats: Arc::default() });
    // HelloAck goes first; the core's cursor and lock state follow it.
    let _ = client.ctrl.send(message(H_HELLO_ACK, &[codec]));
    if let Some(old) = HUB.replace(client.clone()) {
        let _ = old.ctrl.send(message(H_ERROR, b"Another client connected"));
        // Give the Error a moment to arrive: a client that only sees the
        // connection end takes it for a network drop and reconnects, and the
        // two clients then take turns replacing each other.
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(200)).await;
            old.conn.close(2, b"replaced");
        });
        core::stream_stop();
        steam_vrr(0);
    }
    let n = HUB.sessions.fetch_add(1, Ordering::Relaxed) + 1;
    eprintln!(
        "gmstream: client {n} from {peer}: {width}x{height}@{:.3}Hz, codec {codec}, {kbps} kbps{}",
        refresh as f64 / 1000.0,
        if conn.is_udp() && packet > 0 {
            format!(", plain UDP, {packet}-byte packets")
        } else if conn.is_udp() {
            ", plain UDP".to_string()
        } else if datagrams {
            ", video datagrams".to_string()
        } else if one_stream {
            ", one video stream".to_string()
        } else {
            String::new()
        }
    );
    if chunked {
        eprintln!("gmstream: client {n} takes frames in pieces, {packet}-byte chunks");
    }
    if wants_hdr {
        eprintln!("gmstream: client {n} can show HDR10");
    }
    if wants_flush {
        eprintln!("gmstream: client {n} gets each unit padded to {FLUSH_PACKETS} datagrams");
    }
    core::stream_start(
        codec,
        width,
        height,
        refresh,
        kbps,
        packet,
        (if chunked { core::STREAM_CHUNKED } else { 0 })
            | (if wants_hdr { core::STREAM_HDR } else { 0 }),
    );

    let writer = tokio::spawn(async move {
        while let Some(m) = crx.recv().await {
            if send.write_all(&m).await.is_err() {
                break;
            }
        }
    });
    // Pieces of frames go out as they are queued, as many at a time as are
    // waiting (one sendmmsg over plain UDP).
    let chunk_conn = conn.clone();
    let chunker = tokio::spawn(async move {
        while let Some(first) = chrx.recv().await {
            let mut batch = vec![first];
            while batch.len() < 64 {
                match chrx.try_recv() {
                    Ok(d) => batch.push(d),
                    Err(_) => break,
                }
            }
            // Measuring (GM_DEBUG_SEND): a Riptide stripe's ready -> sent.
            let traced = std::env::var_os("GM_DEBUG_SEND").is_some() && batch[0].len() > 27 && batch[0][0] == DATAGRAM_RIPTIDE;
            let encoded = if traced { u64::from_le_bytes(batch[0][19..27].try_into().unwrap()) } else { 0 };
            let start = core::now_ns();
            if chunk_conn.send_datagrams_wait(batch).await.is_err() {
                return;
            }
            if traced {
                SEND_TIMES.lock().unwrap().push((start.saturating_sub(encoded), core::now_ns() - start));
            }
        }
    });
    let video_conn = conn.clone();
    let stats = client.stats.clone();
    let video = tokio::spawn(async move {
        if datagrams {
            // Each frame split into datagrams; no retransmission, a lost
            // fragment loses the frame and the client asks for a keyframe.
            while let Some((id, buf)) = vrx.recv().await {
                if buf[5] == CODEC_PYROWAVE {
                    if send_pyrowave(&video_conn, &buf, flush.as_deref()).await {
                        stats.sent.fetch_add(1, Ordering::Relaxed);
                    }
                    continue;
                }
                let Some(max) = video_conn.max_datagram_size() else { return };
                let chunk = max - VIDEO_FRAGMENT_HEADER;
                let mut ds = Vec::new();
                let count = buf.len().div_ceil(chunk) as u16;
                for (i, part) in buf.chunks(chunk).enumerate() {
                    let mut d = Vec::with_capacity(VIDEO_FRAGMENT_HEADER + part.len());
                    d.push(DATAGRAM_VIDEO);
                    d.extend_from_slice(&id.to_le_bytes());
                    d.extend_from_slice(&(i as u16).to_le_bytes());
                    d.extend_from_slice(&count.to_le_bytes());
                    d.extend_from_slice(part);
                    ds.push(d);
                }
                if let Some(f) = &flush {
                    let n = f.lock().unwrap().add(ds.len(), true);
                    ds.extend(fillers(n));
                }
                if video_conn.send_datagrams_wait(ds).await.is_err() {
                    return;
                }
                stats.sent.fetch_add(1, Ordering::Relaxed);
            }
        } else if one_stream {
            // WebKit grants new streams slowly and stops for good after a
            // reset, so these clients get every frame on one stream.
            let Ok(mut stream) = video_conn.open_uni().await else { return };
            stream.set_priority(i32::MAX);
            while let Some((_, mut buf)) = vrx.recv().await {
                if buf[5] == CODEC_PYROWAVE {
                    if send_pyrowave(&video_conn, &buf, flush.as_deref()).await {
                        stats.sent.fetch_add(1, Ordering::Relaxed);
                    }
                    continue;
                }
                let len = (buf.len() as u32).to_le_bytes();
                buf.splice(0..0, len);
                if stream.write_all(&buf).await.is_err() {
                    return;
                }
                stats.sent.fetch_add(1, Ordering::Relaxed);
            }
        } else {
            while let Some((id, buf)) = vrx.recv().await {
                if buf[5] == CODEC_PYROWAVE {
                    if send_pyrowave(&video_conn, &buf, flush.as_deref()).await {
                        stats.sent.fetch_add(1, Ordering::Relaxed);
                    }
                    continue;
                }
                tokio::spawn(send_frame(video_conn.clone(), stats.clone(), id, buf));
            }
        }
    });
    let monitor_conn = conn.clone();
    let stats = client.stats.clone();
    let monitor = tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(5)).await;
            let [encoded, queue_full, open_timeout, write_timeout, sent] = stats.take();
            if let Some(s) = send_times_summary() {
                eprintln!("gmstream: client {n} PyroWave, last 5 s: {s}");
            }
            if queue_full + open_timeout + write_timeout > 0 {
                eprintln!(
                    "gmstream: client {n} video, last 5 s: {encoded} encoded, {sent} sent, {queue_full} queue full, \
                     {open_timeout} stream open timed out, {write_timeout} write timed out"
                );
            }
            // The client has stopped granting streams: video would stay frozen
            // while the session lives, so end it and let the client reconnect.
            if sent == 0 && open_timeout > 0 {
                eprintln!("gmstream: client {n} stopped accepting video streams; closing");
                monitor_conn.close(3, b"client stopped accepting video streams");
                return;
            }
        }
    });

    if let Conn::Udp(udp) = &*conn {
        udp.accept_mouse();
    }
    let result = control_loop(&mut recv, &client).await;

    writer.abort();
    video.abort();
    chunker.abort();
    monitor.abort();
    if HUB.remove(&client) {
        core::stream_stop();
        steam_vrr(0);
    }
    eprintln!("gmstream: client {n} from {peer} disconnected");
    result
}

async fn control_loop(recv: &mut RecvStream, client: &Client) -> Result<()> {
    loop {
        let (ty, body) = read_message(recv).await?;
        let mut r = Reader(&body);
        match ty {
            C_CONFIGURE => core::stream_configure(r.u16()?, r.u16()?, r.u32()?),
            C_KEYFRAME => core::request_keyframe(),
            C_BITRATE => core::set_bitrate(r.u32()?.clamp(1000, 500_000)),
            // Tiles or slice segments per frame; 0 for the codec's default.
            // The encoder clamps it to what the hardware and the picture allow.
            C_PARALLELISM => core::set_parallelism(r.u16()?.min(256)),
            C_ROTATION => {
                let q = r.u8()?;
                if q == 0 || q == 1 || q == 3 {
                    core::set_rotation(q);
                }
            }
            C_KEY => core::key(r.u16()? as u32, r.u8()? != 0),
            C_BUTTON => core::pointer_button(r.u16()? as u32, r.u8()? != 0),
            C_MOTION => core::pointer_motion(r.f32()?, r.f32()?),
            C_POSITION => core::pointer_position(r.f32()?, r.f32()?),
            C_SCROLL => {
                let (dx, dy) = (r.f32()?, r.f32()?);
                core::pointer_axis(dx, dy, r.i16()? as i32, r.i16()? as i32);
            }
            C_GAMEPAD => {
                let pad = r.u8()?;
                let state = core::GamepadState {
                    buttons: r.u32()?,
                    lx: r.i16()?,
                    ly: r.i16()?,
                    rx: r.i16()?,
                    ry: r.i16()?,
                    lt: r.u8()?,
                    rt: r.u8()?,
                };
                core::gamepad_update(pad, &state);
            }
            C_GAMEPAD_REMOVE => core::gamepad_remove(r.u8()?),
            // u64 anchor_ns (a vblank, host clock), u32 interval_ns.
            C_DISPLAY_GRID => core::set_display_grid(r.u64()?, r.u32()? as u64),
            C_VRR_POLICY => steam_vrr(r.u8()?),
            C_TOUCH => core::touch(r.u8()?, r.u8()?, r.f32()?, r.f32()?),
            C_PING => {
                let mut p = r.u64()?.to_le_bytes().to_vec();
                p.extend_from_slice(&core::now_ns().to_le_bytes());
                let _ = client.ctrl.send(message(H_PONG, &p));
            }
            _ => {} // unknown messages are skipped for forward compatibility
        }
    }
}

/// The client's say over Steam's VRR (protocol VrrPolicy): 0 leaves Steam's
/// toggle alone, 1 turns VRR off (the compositor reports the client's grid as
/// a fixed refresh, so gamescope calls the display VRR-incapable and Steam
/// greys the toggle), 2 turns it on. Steam's toggle only sets gamescope's
/// `adaptive_sync` console variable (through the GAMESCOPE_VRR_ENABLED root
/// property), which gamescopectl sets as well; leaving 2, it goes back to
/// what that property says, Steam's own preference.
fn steam_vrr(policy: u8) {
    static CURRENT: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);
    let policy = policy.min(2);
    core::set_vrr_policy(policy);
    let before = CURRENT.swap(policy, Ordering::Relaxed);
    let script = match (before, policy) {
        (b, 2) if b != 2 => "gamescopectl adaptive_sync 1",
        (2, p) if p != 2 => {
            "v=$(xprop -root GAMESCOPE_VRR_ENABLED 2>/dev/null | grep -o '[01]$'); gamescopectl adaptive_sync \"${v:-0}\""
        }
        _ => return,
    };
    let spawned = std::process::Command::new("sh")
        .args(["-c", script])
        .env("WAYLAND_DISPLAY", "gamescope-0")
        .env("DISPLAY", ":0")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
    match spawned {
        Ok(mut child) => {
            std::thread::spawn(move || child.wait());
        }
        Err(e) => eprintln!("gmstream: Steam VRR: {e}"),
    }
    eprintln!("gmstream: Steam VRR {}", ["left to Steam", "off (fixed refresh)", "on"][policy as usize]);
}
