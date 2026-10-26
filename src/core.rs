//! Bindings to the C core (core/gm.h).

use std::ffi::{CString, c_char, c_int, c_void};
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
    audio: extern "C" fn(*mut c_void, *const u8, usize, u64),
    cursor: extern "C" fn(*mut c_void, *const Cursor),
    pointer_lock: extern "C" fn(*mut c_void, bool),
    rumble: extern "C" fn(*mut c_void, u8, u16, u16, u16),
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
    fn gm_stream_start(codec: c_int, width: c_int, height: c_int, refresh: c_int, kbps: c_int);
    fn gm_stream_configure(width: c_int, height: c_int, refresh: c_int);
    fn gm_stream_stop();
    fn gm_request_keyframe();
    fn gm_set_bitrate(kbps: c_int);
    fn gm_supported_codecs() -> u32;
    fn gm_pointer_motion(dx: f64, dy: f64);
    fn gm_pointer_position(x: f64, y: f64);
    fn gm_pointer_button(button: u32, pressed: bool);
    fn gm_pointer_axis(dx: f64, dy: f64, v120_x: i32, v120_y: i32);
    fn gm_key(key: u32, pressed: bool);
    fn gm_gamepad_update(pad: u8, state: *const GamepadState);
    fn gm_gamepad_remove(pad: u8);
}

/// What the core calls back into; implemented by the session hub.
pub trait Sink: Sync + 'static {
    fn video(&self, packet: &VideoPacket);
    fn audio(&self, opus: &[u8], pts_ns: u64);
    fn cursor(&self, cursor: &Cursor);
    fn pointer_lock(&self, locked: bool);
    fn rumble(&self, pad: u8, strong: u16, weak: u16, duration_ms: u16);
}

static mut SINK: Option<&'static dyn Sink> = None;

fn sink() -> &'static dyn Sink {
    // Set once in run() before the core starts any thread.
    unsafe { (*ptr::addr_of!(SINK)).unwrap() }
}

extern "C" fn on_video(_: *mut c_void, p: *const VideoPacket) {
    sink().video(unsafe { &*p });
}
extern "C" fn on_audio(_: *mut c_void, data: *const u8, len: usize, pts: u64) {
    sink().audio(unsafe { std::slice::from_raw_parts(data, len) }, pts);
}
extern "C" fn on_cursor(_: *mut c_void, c: *const Cursor) {
    sink().cursor(unsafe { &*c });
}
extern "C" fn on_pointer_lock(_: *mut c_void, locked: bool) {
    sink().pointer_lock(locked);
}
extern "C" fn on_rumble(_: *mut c_void, pad: u8, strong: u16, weak: u16, ms: u16) {
    sink().rumble(pad, strong, weak, ms);
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
        audio: on_audio,
        cursor: on_cursor,
        pointer_lock: on_pointer_lock,
        rumble: on_rumble,
    };
    unsafe { gm_run(&c, &cb) }
}

pub fn now_ns() -> u64 {
    unsafe { gm_now_ns() }
}
pub fn heartbeat_ns() -> u64 {
    unsafe { gm_heartbeat_ns() }
}
pub fn stream_start(codec: u8, width: u16, height: u16, refresh_mhz: u32, kbps: u32) {
    unsafe {
        gm_stream_start(codec as c_int, width as c_int, height as c_int, refresh_mhz as c_int, kbps as c_int)
    }
}
pub fn stream_configure(width: u16, height: u16, refresh_mhz: u32) {
    unsafe { gm_stream_configure(width as c_int, height as c_int, refresh_mhz as c_int) }
}
pub fn stream_stop() {
    unsafe { gm_stream_stop() }
}
pub fn request_keyframe() {
    unsafe { gm_request_keyframe() }
}
pub fn set_bitrate(kbps: u32) {
    unsafe { gm_set_bitrate(kbps as c_int) }
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
pub fn gamepad_update(pad: u8, state: &GamepadState) {
    unsafe { gm_gamepad_update(pad, state) }
}
pub fn gamepad_remove(pad: u8) {
    unsafe { gm_gamepad_remove(pad) }
}
