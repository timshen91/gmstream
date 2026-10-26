//! WebTransport server and the client protocol (see PROTOCOL.md).
//!
//! One client streams at a time; a new one replaces the old. Video frames go
//! out each on their own unidirectional stream (newer frames at higher
//! priority, so a frame stuck on loss never delays the next), audio as
//! datagrams, everything else on the client's bidirectional control stream.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use tokio::sync::mpsc;
use wtransport::endpoint::IncomingSession;
use wtransport::quinn::congestion::CubicConfig;
use wtransport::tls::Sha256DigestFmt;
use wtransport::{Connection, Endpoint, Identity, RecvStream, SendStream, ServerConfig, VarInt};

use crate::core;

pub const PROTOCOL_VERSION: u16 = 1;

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
// Host → client.
const H_HELLO_ACK: u8 = 0x81;
const H_CURSOR: u8 = 0x82;
const H_POINTER_LOCK: u8 = 0x83;
const H_RUMBLE: u8 = 0x84;
const H_PONG: u8 = 0x85;
const H_ERROR: u8 = 0x86;

const DATAGRAM_AUDIO: u8 = 1;
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

struct Client {
    conn: Arc<Connection>,
    video: mpsc::Sender<(u32, Vec<u8>)>,
    ctrl: mpsc::UnboundedSender<Vec<u8>>,
    audio_seq: AtomicU32,
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
        if c.video.try_send((p.frame_id, buf)).is_err() {
            // The network side is behind; the client will see a gap.
            core::request_keyframe();
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

    fn pointer_lock(&self, locked: bool) {
        self.control(message(H_POINTER_LOCK, &[locked as u8]));
    }

    fn rumble(&self, pad: u8, strong: u16, weak: u16, duration_ms: u16) {
        let mut p = vec![pad];
        p.extend_from_slice(&strong.to_le_bytes());
        p.extend_from_slice(&weak.to_le_bytes());
        p.extend_from_slice(&duration_ms.to_le_bytes());
        self.control(message(H_RUMBLE, &p));
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

fn server_config(port: u16, identity: Identity) -> ServerConfig {
    let mut transport = wtransport::quinn::TransportConfig::default();
    // LAN: start with a window that fits a large keyframe instead of probing
    // up to it over several round trips.
    let mut cubic = CubicConfig::default();
    cubic.initial_window(4 << 20);
    transport.congestion_controller_factory(Arc::new(cubic));
    transport.send_window(16 << 20);
    ServerConfig::builder()
        .with_bind_default(port)
        .with_custom_transport(identity, transport)
        .keep_alive_interval(Some(Duration::from_secs(2)))
        .max_idle_timeout(Some(Duration::from_secs(10)))
        .expect("idle timeout")
        .build()
}

pub async fn serve(port: u16, token: String, hash: CertHash) -> Result<()> {
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

    let token = Arc::new(token);
    loop {
        let incoming = endpoint.accept().await;
        let token = token.clone();
        tokio::spawn(async move {
            if let Err(e) = handle(incoming, &token).await {
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

async fn send_frame(conn: Arc<Connection>, id: u32, buf: Vec<u8>) {
    let Ok(Ok(opening)) = tokio::time::timeout(FRAME_DEADLINE, conn.open_uni()).await else {
        return;
    };
    let Ok(Ok(mut stream)) = tokio::time::timeout(FRAME_DEADLINE, opening).await else {
        return;
    };
    stream.set_priority(id as i32);
    let sent = tokio::time::timeout(FRAME_DEADLINE, async {
        stream.write_all(&buf).await?;
        stream.finish().await
    })
    .await;
    if sent.is_err() {
        let _ = stream.reset(VarInt::from_u32(1));
    }
}

async fn handle(incoming: IncomingSession, token: &str) -> Result<()> {
    let request = incoming.await?;
    let conn = Arc::new(request.accept().await?);
    let peer = conn.remote_address();
    let (mut send, mut recv) = conn.accept_bi().await?;

    let (ty, body) = read_message(&mut recv).await?;
    let mut r = Reader(&body);
    if ty != C_HELLO {
        bail!("{peer}: expected Hello");
    }
    let version = r.u16()?;
    let (width, height, refresh, kbps) = (r.u16()?, r.u16()?, r.u32()?, r.u32()?);
    let (mask, pref) = (r.u8()?, r.u8()?);
    if version != PROTOCOL_VERSION {
        send_error(&mut send, "Protocol version mismatch; reload the page").await;
        bail!("{peer}: protocol version {version}");
    }
    if r.0 != token.as_bytes() {
        send_error(&mut send, "Wrong or missing token: open the URL gmstream printed").await;
        bail!("{peer}: bad token");
    }
    let both = mask as u32 & core::supported_codecs();
    let codec = if both & (1 << pref) != 0 {
        pref
    } else if let Some(c) = (0..3).find(|c| both & (1 << c) != 0) {
        c
    } else {
        send_error(&mut send, "No video codec in common with this host").await;
        bail!("{peer}: no common codec (client {mask:#x}, host {:#x})", core::supported_codecs());
    };
    let kbps = kbps.clamp(1000, 500_000);

    let (vtx, mut vrx) = mpsc::channel::<(u32, Vec<u8>)>(4);
    let (ctx, mut crx) = mpsc::unbounded_channel::<Vec<u8>>();
    let client = Arc::new(Client { conn: conn.clone(), video: vtx, ctrl: ctx, audio_seq: AtomicU32::new(0) });
    // HelloAck goes first; the core's cursor and lock state follow it.
    let _ = client.ctrl.send(message(H_HELLO_ACK, &[codec]));
    if let Some(old) = HUB.replace(client.clone()) {
        let _ = old.ctrl.send(message(H_ERROR, b"Another client connected"));
        old.conn.close(VarInt::from_u32(2), b"replaced");
        core::stream_stop();
    }
    let n = HUB.sessions.fetch_add(1, Ordering::Relaxed) + 1;
    eprintln!(
        "gmstream: client {n} from {peer}: {width}x{height}@{:.3}Hz, codec {codec}, {kbps} kbps",
        refresh as f64 / 1000.0
    );
    core::stream_start(codec, width, height, refresh, kbps);

    let writer = tokio::spawn(async move {
        while let Some(m) = crx.recv().await {
            if send.write_all(&m).await.is_err() {
                break;
            }
        }
    });
    let video_conn = conn.clone();
    let video = tokio::spawn(async move {
        while let Some((id, buf)) = vrx.recv().await {
            tokio::spawn(send_frame(video_conn.clone(), id, buf));
        }
    });

    let result = control_loop(&mut recv, &client).await;

    writer.abort();
    video.abort();
    if HUB.remove(&client) {
        core::stream_stop();
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
            C_PING => {
                let mut p = r.u64()?.to_le_bytes().to_vec();
                p.extend_from_slice(&core::now_ns().to_le_bytes());
                let _ = client.ctrl.send(message(H_PONG, &p));
            }
            _ => {} // unknown messages are skipped for forward compatibility
        }
    }
}
