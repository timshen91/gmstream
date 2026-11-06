// One stream from the host (apple/Session.swift): the connection, the
// decoders, audio, pacing (phase lock), stats, and the input the app sends.
// The Java side (MainActivity) owns the window, the surface and the input
// devices, and calls in through the JNI functions at the bottom.

#include <jni.h>

#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio.h"
#include "compositor.h"
#include "conn.h"
#include "log.h"
#include "panel.h"
#include "phase.h"
#include "riptide_client.h"
#include "stats.h"
#include "video.h"

namespace gm {

static JavaVM *g_vm;

struct Options {
	std::string host;
	int port = 8444;
	int codec = 2;
	int mbps = 40;
	int width = 1920, height = 1080;
	double refresh_hz = 120;
	bool pieces = true;
	int parallelism = 0, stripes = 0;
	bool keep_awake = true, keep_gpu_awake = true;
	int pacing = 0; // 0 auto (phase lock), 1 low latency
	int rotation = 0; // the display's (Surface.ROTATION_*)
	bool scanout = true; // frames composed into the buffer the panel reads next (compositor.h)
};

class Session : public ConnectionListener {
public:
	Session(const std::string &dir, jobject listener) : id_(dir)
	{
		JNIEnv *env = env_();
		listener_ = env->NewGlobalRef(listener);
		jclass c = env->GetObjectClass(listener);
		m_cursor_ = env->GetMethodID(c, "onCursor", "(IIII[B)V");
		m_relative_ = env->GetMethodID(c, "onRelative", "(Z)V");
		m_rumble_ = env->GetMethodID(c, "onRumble", "(IIII)V");
		m_size_ = env->GetMethodID(c, "onStreamSize", "(II)V");
		m_status_ = env->GetMethodID(c, "onStatus", "(Ljava/lang/String;)V");
		presenter_.on_presented = [this](const FrameTiming &t) {
			stages_.add(t);
			phase_.presented(to_latch_grid(t));
		};
		presenter_.on_dropped = [this](const FrameTiming &t) {
			stages_.drop();
			phase_.dropped(to_latch_grid(t));
		};
		// A torn frame missed its transfer, as far as the lock is concerned:
		// it steers the host's frames earlier until they stop tearing.
		compositor_.on_presented = presenter_.on_presented;
		compositor_.on_slack = [this](double ns) { phase_.slack(ns); };
		compositor_.on_dropped = presenter_.on_dropped;
		compositor_.on_torn = [this](const FrameTiming &t, int row) {
			stages_.tear(row);
			FrameTiming m = to_latch_grid(t);
			m.presented = 0;
			phase_.dropped(m);
		};
		video_.on_size = [this](int w, int h) { stream_size(w, h); };
		riptide_.on_dropped = [this](const FrameTiming &t) { stages_.drop(); phase_.dropped(t); };
		riptide_.on_size = [this](int w, int h) { stream_size(w, h); };
		riptide_.set_presenter(&presenter_);
		riptide_.set_compositor(&compositor_, compositor_.gpu());
		riptide_.on_loss = [this](uint32_t id, int count, const std::vector<std::pair<int, int>> &r) {
			std::lock_guard<std::mutex> g(conn_lock_);
			if (conn_)
				conn_->riptide_loss(id, count, r);
		};
		riptide_.on_need_intra = [this] { send(msg::keyframe); };
		riptide_.on_gap = [this] { through_.gap(); };
		LOGI("this device's key: %s", id_.authorized_line("android").c_str());
	}

	~Session() override
	{
		stop();
		env_()->DeleteGlobalRef(listener_);
	}

	Identity &identity() { return id_; }

	void start(const Options &o)
	{
		stop();
		opt_ = o;
		running_ = true;
		phase_.restart(o.refresh_hz);
		presenter_.set_schedule(PanelSchedule::load(o.refresh_hz));
		ticker_ = std::thread([this] { run(); });
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> g(tick_mx_);
			running_ = false;
		}
		tick_cv_.notify_all();
		if (ticker_.joinable())
			ticker_.join();
		disconnect();
	}

	void set_window(ANativeWindow *w)
	{
		presenter_.set_window(w);
		video_.set_enabled(w != nullptr);
		riptide_.set_window(w);
		if (w)
			send(msg::keyframe);
	}

	void send(uint8_t type, const std::vector<uint8_t> &payload = {})
	{
		std::lock_guard<std::mutex> g(conn_lock_);
		if (conn_)
			conn_->send(type, payload);
	}
	void motion(double dx, double dy)
	{
		std::lock_guard<std::mutex> g(conn_lock_);
		if (conn_)
			conn_->motion(dx, dy);
	}
	void position(float x, float y)
	{
		last_x_ = x;
		last_y_ = y;
		has_position_ = true;
		std::lock_guard<std::mutex> g(conn_lock_);
		if (conn_)
			conn_->position(x, y);
	}
	void button(int code, bool down)
	{
		// Buttons are ordered and the mouse isn't: click where the mouse is.
		if (!relative_ && has_position_) {
			Writer p;
			p.f32(last_x_);
			p.f32(last_y_);
			send(msg::position, p.d);
		}
		Writer m;
		m.u16(uint16_t(code));
		m.u8(down ? 1 : 0);
		send(msg::button, m.d);
	}
	void set_bitrate(int mbps)
	{
		opt_.mbps = mbps;
		Writer m;
		m.u32(uint32_t(mbps) * 1000);
		send(msg::bitrate, m.d);
	}

	std::string stats();

	// ConnectionListener.
	void on_frame(Frame &&f) override;
	void on_audio(const uint8_t *p, size_t n, uint64_t) override
	{
		if (audio_)
			audio_->push(p, n);
	}
	void on_riptide(const RiptidePacket &p) override
	{
		through_.bytes(p.size);
		int64_t off = 0;
		bool known;
		{
			std::lock_guard<std::mutex> g(conn_lock_);
			known = conn_ && conn_->clock_offset(off);
		}
		riptide_.packet(p, known, off);
	}
	void on_hello_ack(uint8_t codec) override
	{
		stream_codec_ = codec;
		static const char *names[] = {"H.264", "HEVC", "AV1", "PyroWave", "Riptide"};
		codec_name_ = names[std::min<int>(codec, 4)];
		LOGI("hello ack: %s", codec_name_.c_str());
	}
	void on_cursor(int w, int h, int hx, int hy, const uint8_t *rgba, size_t n) override
	{
		JNIEnv *env = env_();
		jbyteArray a = env->NewByteArray(jsize(n));
		env->SetByteArrayRegion(a, 0, jsize(n), reinterpret_cast<const jbyte *>(rgba));
		env->CallVoidMethod(listener_, m_cursor_, w, h, hx, hy, a);
		env->DeleteLocalRef(a);
	}
	void on_relative(bool on) override
	{
		relative_ = on;
		env_()->CallVoidMethod(listener_, m_relative_, jboolean(on));
	}
	void on_rumble(int i, uint16_t s, uint16_t w, int ms) override
	{
		env_()->CallVoidMethod(listener_, m_rumble_, i, int(s), int(w), ms);
	}
	void on_encode_format(const std::string &s) override
	{
		std::lock_guard<std::mutex> g(text_lock_);
		host_format_ = s;
	}
	void on_closed(const std::string &why) override
	{
		{
			std::lock_guard<std::mutex> g(text_lock_);
			closed_reason_ = why;
		}
		closed_ = true;
		LOGW("closed: %s", why.c_str());
		tick_cv_.notify_all();
	}

private:
	JNIEnv *env_()
	{
		JNIEnv *env = nullptr;
		if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
			g_vm->AttachCurrentThread(&env, nullptr); // a thread of ours: stays attached until it ends
		return env;
	}
	void status(const std::string &s)
	{
		JNIEnv *env = env_();
		jstring j = env->NewStringUTF(s.c_str());
		env->CallVoidMethod(listener_, m_status_, j);
		env->DeleteLocalRef(j);
	}
	void stream_size(int w, int h)
	{
		// Scanout mode: the compositor, for pictures of this size (the
		// decoders call this before their first frame of a new size).
		// A Riptide picture that comes taller than wide is the one we asked to
		// be turned to the panel (connect): streamed, a stripe at a time.
		bool turned = opt_.scanout && stream_codec_ == 4 && (opt_.rotation == 1 || opt_.rotation == 3) && h > w;
		if (opt_.scanout && !compositor_.start(w, h, opt_.rotation, turned))
			LOGW("scanout: no compositor, frames go through SurfaceFlinger");
		if (turned)
			std::swap(w, h); // the picture's own orientation, for the window
		if (w == stream_w_ && h == stream_h_)
			return;
		stream_w_ = w;
		stream_h_ = h;
		env_()->CallVoidMethod(listener_, m_size_, w, h);
	}
	// The phase lock steers frames to the cut-off that decides when they are
	// shown. On macOS that is the refresh; on Android it is SurfaceFlinger's
	// latch, a fixed time (its "sf duration", 10.3 ms on the Odin 3) before
	// the refresh the frame then shows at, and a frame that misses it by a
	// little waits a whole period. So the lock sees each frame as presented at
	// its latch point plus one period: its lead is then the time to the next
	// latch, and "a refresh after its flip point" means it made that latch.
	// The latch point comes from the present time (exact, on the refresh grid)
	// minus the median latch -> present time.
	FrameTiming to_latch_grid(FrameTiming t)
	{
		// A frame is ready when its pixels are, not when the CPU handed it
		// over: Riptide's last stripe and RGB pass are still on the GPU then.
		// (Streamed frames say when they were ready themselves: when stripe 0
		// would have had to be done for every stripe to make its rows.)
		if (!compositor_.streaming() && t.decoded > t.submitted)
			t.submitted = t.decoded;
		if (compositor_.running()) {
			// Scanout: the cut-off is the compositor's sample point for a
			// refresh (latched), the same distance before every transfer.
			if (t.presented && t.latched)
				t.presented = t.latched + uint64_t(1e9 / opt_.refresh_hz);
			return t;
		}
		std::lock_guard<std::mutex> g(pipe_lock_);
		if (t.latched && t.presented > t.latched) {
			pipe_.push_back(double(t.presented - t.latched));
			if (pipe_.size() > 120)
				pipe_.erase(pipe_.begin());
		}
		if (pipe_.size() < 10 || !t.presented)
			return t; // scanout mode: the cut-off is the refresh itself
		std::vector<double> s = pipe_;
		std::nth_element(s.begin(), s.begin() + long(s.size() / 2), s.end());
		double period = 1e9 / opt_.refresh_hz;
		t.presented = uint64_t(double(t.presented) - s[s.size() / 2] + period);
		return t;
	}
	std::mutex pipe_lock_;
	std::vector<double> pipe_; // recent latch -> present, ns

	void connect();
	void disconnect();
	void send_grid();
	void run();

	Identity id_;
	jobject listener_;
	jmethodID m_cursor_, m_relative_, m_rumble_, m_size_, m_status_;
	Options opt_;
	std::mutex conn_lock_;
	std::unique_ptr<Connection> conn_;
	Presenter presenter_;
	Compositor compositor_{presenter_}; // owns the GPU: before Riptide, which uses it
	VideoDecoder video_{presenter_, compositor_};
	RiptideClient riptide_;
	std::unique_ptr<AudioPlayer> audio_;
	PhaseLock phase_{120};
	StageStats stages_;
	Throughput through_;
	FrameGate gate_;
	std::thread ticker_;
	std::mutex tick_mx_;
	std::condition_variable tick_cv_;
	bool running_ = false;
	std::atomic<bool> closed_{false};
	std::mutex text_lock_;
	std::string closed_reason_, host_format_, codec_name_ = "-";
	uint8_t stream_codec_ = 255;
	std::atomic<bool> relative_{false};
	float last_x_ = 0, last_y_ = 0;
	bool has_position_ = false;
	int stream_w_ = 0, stream_h_ = 0;
};

void Session::on_frame(Frame &&f)
{
	through_.bytes(f.bits.size());
	bool request = false;
	if (!gate_.admit(f.id, f.key, request, through_)) {
		if (request)
			send(msg::keyframe);
		return;
	}
	FrameTiming t;
	t.encode_ns = f.encoded_ns - f.slot_ns;
	t.present_host = f.slot_ns;
	{
		std::lock_guard<std::mutex> g(conn_lock_);
		if (conn_ && conn_->to_local(f.slot_ns, t.present_local) && conn_->to_local(f.encoded_ns, t.encoded_local))
			t.local = true;
	}
	t.received = f.received_ns;
	t.first_received = f.first_ns;
	t.overlap_ns = f.overlap_ns;
	if (!video_.decode(std::move(f), t)) {
		gate_.reset();
		send(msg::keyframe);
	}
}

void Session::connect()
{
	gate_.reset();
	closed_ = false;
	{
		std::lock_guard<std::mutex> g(text_lock_);
		closed_reason_.clear();
		host_format_.clear();
	}
	auto c = std::make_unique<Connection>(id_, *this);
	std::string err;
	status("Connecting to " + opt_.host + "…");
	if (!c->open(opt_.host, uint16_t(opt_.port), err)) {
		on_closed(err);
		return;
	}
	HelloParams p;
	p.width = opt_.width & ~1;
	p.height = opt_.height & ~1;
	p.refresh_mhz = uint32_t(opt_.refresh_hz * 1000);
	p.kbps = uint32_t(opt_.mbps) * 1000;
	p.codec_mask = 0b111 | (riptide_.available() ? 1 << 4 : 0);
	p.preferred = uint8_t(opt_.codec);
	p.pieces = opt_.pieces;
	{
		std::lock_guard<std::mutex> g(conn_lock_);
		conn_ = std::move(c);
		conn_->start(p);
		conn_->keep_awake(opt_.keep_awake);
	}
	riptide_.set_keep_gpu_awake(opt_.keep_gpu_awake);

	riptide_.set_rotation(opt_.rotation);
	// Scanout starts with the stream's size (stream_size).
	compositor_.stop();
	stream_w_ = stream_h_ = 0;
	riptide_.reset();
	video_.reset();
	if (!audio_)
		audio_ = std::make_unique<AudioPlayer>();
	send(msg::vrr_policy, {uint8_t(opt_.pacing == 1 ? 2 : 1)});
	Writer par;
	int parts = (opt_.codec == 1 || opt_.codec == 2) ? opt_.parallelism : opt_.codec == 4 ? opt_.stripes : 0;
	par.u16(uint16_t(std::clamp(parts, 0, 65535)));
	send(msg::parallelism, par.d);
	// Scanout on a panel that scans the picture's columns: Riptide's picture
	// turned so that its stripes are bands of scan lines (streamed into the
	// buffer on screen as each is decoded). Other codecs ignore it.
	if (opt_.scanout && (opt_.rotation == 1 || opt_.rotation == 3))
		send(msg::rotation, {uint8_t(opt_.rotation)});
	status("");
}

void Session::disconnect()
{
	std::unique_ptr<Connection> c;
	{
		std::lock_guard<std::mutex> g(conn_lock_);
		c = std::move(conn_);
	}
	if (c)
		c->stop();
}

void Session::send_grid()
{
	Writer m;
	if (opt_.pacing == 1) {
		m.u64(0);
		m.u32(0);
	} else {
		uint64_t anchor, interval, host;
		if (!phase_.step(anchor, interval))
			return;
		{
			std::lock_guard<std::mutex> g(conn_lock_);
			if (!conn_ || !conn_->to_host(anchor, host))
				return;
		}
		if (phase_.take_drain())
			presenter_.skip_one();
		m.u64(host);
		m.u32(uint32_t(interval));
	}
	send(msg::display_grid, m.d);
}

void Session::run()
{
	connect();
	uint64_t next_ping = now_ns(), next_stats = now_ns() + 1000000000ull;
	std::unique_lock<std::mutex> lk(tick_mx_);
	while (running_) {
		tick_cv_.wait_for(lk, std::chrono::milliseconds(50));
		if (!running_)
			break;
		uint64_t now = now_ns();
		if (closed_) {
			std::string why;
			{
				std::lock_guard<std::mutex> g(text_lock_);
				why = closed_reason_;
			}
			lk.unlock();
			disconnect();
			status(opt_.host + ": " + why);
			bool replaced = why == "Another client connected";
			lk.lock();
			if (replaced) {
				tick_cv_.wait(lk, [&] { return !running_; });
				break;
			}
			tick_cv_.wait_for(lk, std::chrono::seconds(2), [&] { return !running_; });
			if (!running_)
				break;
			lk.unlock();
			connect();
			lk.lock();
			continue;
		}
		if (now >= next_ping) {
			lk.unlock();
			{
				std::lock_guard<std::mutex> g(conn_lock_);
				if (conn_)
					conn_->ping();
			}
			send_grid();
			lk.lock();
			next_ping = now + 500000000ull;
		}
		if (now >= next_stats)
			next_stats = now + 1000000000ull;
	}
}

static std::string fmt(bool has, double v)
{
	if (!has)
		return "-";
	char b[32];
	snprintf(b, sizeof(b), "%.1f", v);
	return b;
}

std::string Session::stats()
{
	std::string reason, format, codec;
	{
		std::lock_guard<std::mutex> g(text_lock_);
		reason = closed_reason_;
		format = host_format_;
		codec = codec_name_;
	}
	if (closed_ && !reason.empty())
		return opt_.host + " — " + reason;
	auto st = stages_.take();
	double mbps;
	int gaps;
	through_.take(mbps, gaps);
	char line[256];
	std::string out;
	snprintf(line, sizeof(line), "%dx%d %s %d fps shown %.0f Mbps\n", stream_w_, stream_h_, codec.c_str(), st.shown, mbps);
	out += line;
	snprintf(line, sizeof(line), "%-11s%-7s%-7s%s\n", "ms, 5 s", "p50", "p99", "max");
	out += line;
	bool overlaps = ((stream_codec_ == 1 || stream_codec_ == 2) && opt_.pieces) || stream_codec_ == 4;
	for (auto &r : st.rows) {
		if (r.name == "overlap" && !overlaps)
			continue;
		if (r.name == "to Android" || r.name == "total" || r.name == "spacing")
			out += "────────────────────────────\n";
		snprintf(line, sizeof(line), "%-11s%-7s%-7s%s\n", r.name.c_str(), fmt(r.has, r.p50).c_str(),
		         fmt(r.has, r.p99).c_str(), fmt(r.has, r.max).c_str());
		out += line;
	}
	out += (opt_.pacing == 1 ? std::string("pacing low latency, phase: off") : "pacing auto (fixed refresh: phase lock), " + phase_.summary()) + "\n";
	if (!format.empty())
		out += "host in  " + format + "\n";
	out += "here in  " + (stream_codec_ == 4 ? riptide_.summary() : video_.summary()) + "\n";
	uint64_t rtt = 0;
	{
		std::lock_guard<std::mutex> g(conn_lock_);
		if (conn_)
			rtt = conn_->rtt_ns();
	}
	if (compositor_.running()) {
		out += presenter_.schedule().summary() + "\n";
		out += presenter_.ring_summary() + "\n" + compositor_.summary() + "\n";
		snprintf(line, sizeof(line), "scanout: torn %d", st.torn);
		out += line;
		if (st.torn) {
			snprintf(line, sizeof(line), " (rows %d-%d of %d)", st.tear_min, st.tear_max, presenter_.schedule().rows);
			out += line;
		}
		out += "\n";
	}
	snprintf(line, sizeof(line), "rtt %.1f ms  dropped %d  gaps %d\naudio %s", rtt / 1e6, st.dropped, gaps,
	         audio_ && audio_->ok() ? (std::to_string(int(audio_->buffered_ms())) + " ms buffered").c_str() : "no output");
	out += line;
	return out;
}

} // namespace gm

// ---- JNI ----------------------------------------------------------------------

using gm::Session;

static Session *S(jlong h) { return reinterpret_cast<Session *>(h); }
static std::string str(JNIEnv *env, jstring s)
{
	if (!s)
		return {};
	const char *c = env->GetStringUTFChars(s, nullptr);
	std::string r(c);
	env->ReleaseStringUTFChars(s, c);
	return r;
}

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *)
{
	gm::g_vm = vm;
	return JNI_VERSION_1_6;
}

#define FN(ret, name) JNIEXPORT ret JNICALL Java_org_gmstream_Native_##name

FN(jlong, create)(JNIEnv *env, jclass, jstring dir, jobject listener)
{
	return reinterpret_cast<jlong>(new Session(str(env, dir), listener));
}

FN(void, destroy)(JNIEnv *, jclass, jlong h) { delete S(h); }

FN(jstring, authorizedLine)(JNIEnv *env, jclass, jlong h, jstring device)
{
	return env->NewStringUTF(S(h)->identity().authorized_line(str(env, device)).c_str());
}

FN(void, forgetHosts)(JNIEnv *, jclass, jlong h) { S(h)->identity().forget_hosts(); }

FN(void, start)(JNIEnv *env, jclass, jlong h, jstring host, jint port, jint codec, jint mbps, jint w, jint hgt,
                jdouble hz, jboolean pieces, jint parallelism, jint stripes, jboolean awake, jboolean gpu_awake,
                jint pacing, jboolean scanout, jint rotation)
{
	gm::Options o;
	o.host = str(env, host);
	o.port = port;
	o.codec = codec;
	o.mbps = mbps;
	o.width = w;
	o.height = hgt;
	o.refresh_hz = hz;
	o.pieces = pieces;
	o.parallelism = parallelism;
	o.stripes = stripes;
	o.keep_awake = awake;
	o.keep_gpu_awake = gpu_awake;
	o.pacing = pacing;
	o.scanout = scanout;
	o.rotation = rotation;
	S(h)->start(o);
}

FN(void, stop)(JNIEnv *, jclass, jlong h) { S(h)->stop(); }

FN(void, setSurface)(JNIEnv *env, jclass, jlong h, jobject surface)
{
	ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
	S(h)->set_window(w);
	if (w)
		ANativeWindow_release(w); // set_window took its own references
}

FN(jstring, stats)(JNIEnv *env, jclass, jlong h) { return env->NewStringUTF(S(h)->stats().c_str()); }

FN(void, key)(JNIEnv *, jclass, jlong h, jint code, jboolean down)
{
	gm::Writer m;
	m.u16(uint16_t(code));
	m.u8(down ? 1 : 0);
	S(h)->send(gm::msg::key, m.d);
}

FN(void, button)(JNIEnv *, jclass, jlong h, jint code, jboolean down) { S(h)->button(code, down); }
FN(void, motion)(JNIEnv *, jclass, jlong h, jdouble dx, jdouble dy) { S(h)->motion(dx, dy); }
FN(void, position)(JNIEnv *, jclass, jlong h, jfloat x, jfloat y) { S(h)->position(x, y); }

FN(void, scroll)(JNIEnv *, jclass, jlong h, jfloat dx, jfloat dy)
{
	// In notches (positive: down or right), sent as the web client does.
	gm::Writer m;
	m.f32(dx * 15);
	m.f32(dy * 15);
	m.i16(int16_t(std::clamp<long>(std::lround(dx * 120), -32768, 32767)));
	m.i16(int16_t(std::clamp<long>(std::lround(dy * 120), -32768, 32767)));
	S(h)->send(gm::msg::scroll, m.d);
}

FN(void, touch)(JNIEnv *, jclass, jlong h, jint id, jint phase, jfloat x, jfloat y)
{
	gm::Writer m;
	m.u8(uint8_t(id));
	m.u8(uint8_t(phase));
	m.f32(std::clamp(x, 0.f, 1.f));
	m.f32(std::clamp(y, 0.f, 1.f));
	S(h)->send(gm::msg::touch, m.d);
}

FN(void, gamepad)(JNIEnv *, jclass, jlong h, jint index, jint buttons, jint lx, jint ly, jint rx, jint ry, jint lt,
                  jint rt)
{
	gm::Writer m;
	m.u8(uint8_t(index));
	m.u32(uint32_t(buttons));
	m.i16(int16_t(lx));
	m.i16(int16_t(ly));
	m.i16(int16_t(rx));
	m.i16(int16_t(ry));
	m.u8(uint8_t(lt));
	m.u8(uint8_t(rt));
	S(h)->send(gm::msg::gamepad, m.d);
}

FN(void, gamepadRemove)(JNIEnv *, jclass, jlong h, jint index) { S(h)->send(gm::msg::gamepad_remove, {uint8_t(index)}); }
FN(void, setBitrate)(JNIEnv *, jclass, jlong h, jint mbps) { S(h)->set_bitrate(mbps); }
FN(void, requestKeyframe)(JNIEnv *, jclass, jlong h) { S(h)->send(gm::msg::keyframe); }

} // extern "C"
