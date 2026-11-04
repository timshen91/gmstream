//! Bindings to the C core (core/gm.h).

use std::ffi::{CString, c_char, c_int, c_uint, c_void};
use std::ptr;

#[repr(C)]
struct GmConfig {
    width: c_int,
    height: c_int,
    refresh_mhz: c_int,
    stats: bool,
    debug: bool,
    socket: *const c_char,
    child_argv: *mut *mut c_char,
}

#[repr(C)]
pub struct VideoPacket {
    data: *const u8,
    len: usize,
    pub frame_id: u32,
    pub slot_ns: u64,
    pub encoded_ns: u64,
    pub width: u16,
    pub height: u16,
    pub keyframe: bool,
    pub codec: c_int,
}

impl VideoPacket {
    pub fn data(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.data, self.len) }
    }
}

/// One piece of a frame's bitstream (core/gm.h).
#[repr(C)]
pub struct VideoChunk {
    data: *const u8,
    len: usize,
    pub frame_id: u32,
    pub offset: u32,
    pub total: u32,
    pub index: u16,
    pub count: u16,
    pub slot_ns: u64,
    pub encoded_ns: u64,
    /// When the frame's first piece went out (last piece only).
    pub first_ns: u64,
    pub width: u16,
    pub height: u16,
    pub keyframe: bool,
    pub last: bool,
    pub codec: c_int,
}

impl VideoChunk {
    pub fn data(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.data, self.len) }
    }
}

/// A stripe of a Riptide frame (core/gm.h).
#[repr(C)]
pub struct RiptidePackets {
    data: *const u8,
    len: usize,
    pub frame_id: u32,
    pub first: u16,
    pub count: u16,
    pub stripe: u8,
    pub stripes: u8,
    pub first_rows: u16,
    pub stripe_rows: u16,
    pub inter: u8,
    pub predicted: bool,
    pub repaired: u32,
    pub slot_ns: u64,
    pub encoded_ns: u64,
    pub width: u16,
    pub height: u16,
}

impl RiptidePackets {
    /// The packets, each after a u16 length.
    pub fn data(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.data, self.len) }
    }
}

#[repr(C)]
pub struct Cursor {
    rgba: *const u8,
    pub width: u16,
    pub height: u16,
    pub hotspot_x: i16,
    pub hotspot_y: i16,
}

impl Cursor {
    /// Straight-alpha RGBA pixels; None when the cursor is hidden.
    pub fn rgba(&self) -> Option<&[u8]> {
        if self.rgba.is_null() {
            return None;
        }
        let len = self.width as usize * self.height as usize * 4;
        Some(unsafe { std::slice::from_raw_parts(self.rgba, len) })
    }
}

#[repr(C)]
struct GmCallbacks {
    ctx: *mut c_void,
    video: extern "C" fn(*mut c_void, *const VideoPacket),
    video_chunk: extern "C" fn(*mut c_void, *const VideoChunk),
    audio: extern "C" fn(*mut c_void, *const u8, usize, u64),
    cursor: extern "C" fn(*mut c_void, *const Cursor),
    relative: extern "C" fn(*mut c_void, bool),
    cursor_clip: extern "C" fn(*mut c_void, u16, u16, u16, u16),
    rumble: extern "C" fn(*mut c_void, u8, u16, u16, u16),
    encode_format: extern "C" fn(*mut c_void, *const std::os::raw::c_char),
    riptide: extern "C" fn(*mut c_void, *const RiptidePackets),
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GamepadState {
    pub buttons: u32,
    pub lx: i16,
    pub ly: i16,
    pub rx: i16,
    pub ry: i16,
    pub lt: u8,
    pub rt: u8,
}

unsafe extern "C" {
    fn gm_run(config: *const GmConfig, cb: *const GmCallbacks) -> c_int;
    fn gm_now_ns() -> u64;
    fn gm_heartbeat_ns() -> u64;
    fn gm_stream_start(
        codec: c_int,
        width: c_int,
        height: c_int,
        refresh: c_int,
        kbps: c_int,
        packet: c_int,
        flags: c_int,
    );
    fn gm_stream_configure(width: c_int, height: c_int, refresh: c_int);
    fn gm_stream_stop();
    fn gm_request_keyframe();
    fn gm_set_display_grid(anchor_ns: u64, interval_ns: u64);
    fn gm_set_vrr_policy(policy: c_int);
    fn gm_set_bitrate(kbps: c_int);
    fn gm_set_parallelism(parts: c_uint);
    fn gm_supported_codecs() -> u32;
    fn gm_pointer_motion(dx: f64, dy: f64);
    fn gm_pointer_position(x: f64, y: f64);
    fn gm_pointer_button(button: u32, pressed: bool);
    fn gm_pointer_axis(dx: f64, dy: f64, v120_x: i32, v120_y: i32);
    fn gm_key(key: u32, pressed: bool);
    fn gm_touch(id: i32, phase: c_int, x: f64, y: f64);
    fn gm_gamepad_update(pad: u8, state: *const GamepadState);
    fn gm_gamepad_remove(pad: u8);
    fn gm_riptide_loss(frame_id: u32, count: u16, ranges: *const u16, n_ranges: usize);
}

/// What the core calls back into; implemented by the session hub.
pub trait Sink: Sync + 'static {
    fn video(&self, packet: &VideoPacket);
    fn video_chunk(&self, chunk: &VideoChunk);
    fn audio(&self, opus: &[u8], pts_ns: u64);
    fn cursor(&self, cursor: &Cursor);
    fn relative(&self, relative: bool);
    fn cursor_clip(&self, clip: [u16; 4]);
    fn rumble(&self, pad: u8, strong: u16, weak: u16, duration_ms: u16);
    fn encode_format(&self, text: &str);
    fn riptide(&self, packets: &RiptidePackets);
}

static mut SINK: Option<&'static dyn Sink> = None;

fn sink() -> &'static dyn Sink {
    // Set once in run() before the core starts any thread.
    unsafe { (*ptr::addr_of!(SINK)).unwrap() }
}

extern "C" fn on_video(_: *mut c_void, p: *const VideoPacket) {
    sink().video(unsafe { &*p });
}
extern "C" fn on_video_chunk(_: *mut c_void, c: *const VideoChunk) {
    sink().video_chunk(unsafe { &*c });
}
extern "C" fn on_riptide(_: *mut c_void, p: *const RiptidePackets) {
    sink().riptide(unsafe { &*p });
}
extern "C" fn on_audio(_: *mut c_void, data: *const u8, len: usize, pts: u64) {
    sink().audio(unsafe { std::slice::from_raw_parts(data, len) }, pts);
}
extern "C" fn on_cursor(_: *mut c_void, c: *const Cursor) {
    sink().cursor(unsafe { &*c });
}
extern "C" fn on_relative(_: *mut c_void, relative: bool) {
    sink().relative(relative);
}
extern "C" fn on_cursor_clip(_: *mut c_void, left: u16, top: u16, right: u16, bottom: u16) {
    sink().cursor_clip([left, top, right, bottom]);
}
extern "C" fn on_rumble(_: *mut c_void, pad: u8, strong: u16, weak: u16, ms: u16) {
    sink().rumble(pad, strong, weak, ms);
}
extern "C" fn on_encode_format(_: *mut c_void, text: *const std::os::raw::c_char) {
    if text.is_null() {
        return;
    }
    // The core owns the string only for this call.
    let text = unsafe { std::ffi::CStr::from_ptr(text) };
    if let Ok(text) = text.to_str() {
        sink().encode_format(text);
    }
}

pub struct Config {
    pub width: i32,
    pub height: i32,
    pub refresh_mhz: i32,
    pub stats: bool,
    pub debug: bool,
    pub socket: Option<String>,
    pub child: Vec<String>,
}

/// Runs the compositor on this thread until the child exits.
pub fn run(config: &Config, sink: &'static dyn Sink) -> i32 {
    unsafe { SINK = Some(sink) };
    let args: Vec<CString> = config
        .child
        .iter()
        .map(|a| CString::new(a.as_str()).expect("NUL in argument"))
        .collect();
    let mut argv: Vec<*mut c_char> = args.iter().map(|a| a.as_ptr() as *mut c_char).collect();
    argv.push(ptr::null_mut());
    let socket = config.socket.as_deref().map(|s| CString::new(s).expect("NUL in socket name"));
    let c = GmConfig {
        width: config.width,
        height: config.height,
        refresh_mhz: config.refresh_mhz,
        stats: config.stats,
        debug: config.debug,
        socket: socket.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
        child_argv: if config.child.is_empty() { ptr::null_mut() } else { argv.as_mut_ptr() },
    };
    let cb = GmCallbacks {
        ctx: ptr::null_mut(),
        video: on_video,
        video_chunk: on_video_chunk,
        audio: on_audio,
        cursor: on_cursor,
        relative: on_relative,
        cursor_clip: on_cursor_clip,
        encode_format: on_encode_format,
        rumble: on_rumble,
        riptide: on_riptide,
    };
    unsafe { gm_run(&c, &cb) }
}

pub fn now_ns() -> u64 {
    unsafe { gm_now_ns() }
}
pub fn heartbeat_ns() -> u64 {
    unsafe { gm_heartbeat_ns() }
}
/// Send each frame in pieces as the encoder writes it (AV1 only).
pub const STREAM_CHUNKED: u32 = 1 << 0;
pub const STREAM_HDR: u32 = 1 << 1;

/// Riptide: the client missed these packets of a frame. `count`: the frame's
/// packets as the client knows them (0: unknown); `ranges`: (first, length)
/// pairs.
pub fn riptide_loss(frame_id: u32, count: u16, ranges: &[u16]) {
    unsafe { gm_riptide_loss(frame_id, count, ranges.as_ptr(), ranges.len() / 2) }
}

/// `packet`: the most PyroWave or video chunk payload per datagram (0: the
/// default). `flags`: STREAM_* above.
pub fn stream_start(
    codec: u8,
    width: u16,
    height: u16,
    refresh_mhz: u32,
    kbps: u32,
    packet: usize,
    flags: u32,
) {
    unsafe {
        gm_stream_start(
            codec as c_int,
            width as c_int,
            height as c_int,
            refresh_mhz as c_int,
            kbps as c_int,
            packet as c_int,
            flags as c_int,
        )
    }
}
pub fn stream_configure(width: u16, height: u16, refresh_mhz: u32) {
    unsafe { gm_stream_configure(width as c_int, height as c_int, refresh_mhz as c_int) }
}
pub fn stream_stop() {
    unsafe { gm_stream_stop() }
}
pub fn set_display_grid(anchor_ns: u64, interval_ns: u64) {
    unsafe { gm_set_display_grid(anchor_ns, interval_ns) }
}
pub fn set_vrr_policy(policy: u8) {
    unsafe { gm_set_vrr_policy(policy as c_int) }
}

pub fn request_keyframe() {
    unsafe { gm_request_keyframe() }
}
pub fn set_bitrate(kbps: u32) {
    unsafe { gm_set_bitrate(kbps as c_int) }
}

/// Tiles (AV1) or slice segments (HEVC) per frame; 0 for the codec's default.
pub fn set_parallelism(parts: u16) {
    unsafe { gm_set_parallelism(parts as c_uint) }
}
pub fn supported_codecs() -> u32 {
    unsafe { gm_supported_codecs() }
}
pub fn pointer_motion(dx: f64, dy: f64) {
    unsafe { gm_pointer_motion(dx, dy) }
}
pub fn pointer_position(x: f64, y: f64) {
    unsafe { gm_pointer_position(x, y) }
}
pub fn pointer_button(button: u32, pressed: bool) {
    unsafe { gm_pointer_button(button, pressed) }
}
pub fn pointer_axis(dx: f64, dy: f64, v120_x: i32, v120_y: i32) {
    unsafe { gm_pointer_axis(dx, dy, v120_x, v120_y) }
}
pub fn key(code: u32, pressed: bool) {
    unsafe { gm_key(code, pressed) }
}
/// Phase 0 down, 1 motion, 2 up, 3 cancel (every touch ends).
pub fn touch(id: u8, phase: u8, x: f64, y: f64) {
    unsafe { gm_touch(id as i32, phase as c_int, x.clamp(0.0, 1.0), y.clamp(0.0, 1.0)) }
}
pub fn gamepad_update(pad: u8, state: &GamepadState) {
    unsafe { gm_gamepad_update(pad, state) }
}
pub fn gamepad_remove(pad: u8) {
    unsafe { gm_gamepad_remove(pad) }
}
