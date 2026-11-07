// The gmstream protocol over plain UDP (PROTOCOL.md, "Plain UDP"), as the
// Apple clients speak it (apple/Connection.swift): the handshake and Auth,
// the sealed control stream (numbered datagrams, acknowledged and resent),
// the mouse and Riptide loss datagrams, keep-awake, video in fragments or in
// pieces, PyroWave and Riptide packets, and audio. Portable C++ (POSIX
// sockets), so it builds and is tested on the build machine as well.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"

namespace gm {

uint64_t now_ns(); // CLOCK_MONOTONIC

namespace msg {
// Client -> host.
constexpr uint8_t hello = 0x01, configure = 0x02, keyframe = 0x03, bitrate = 0x04, key = 0x05, button = 0x06,
                  motion = 0x07, position = 0x08, scroll = 0x09, gamepad = 0x0a, gamepad_remove = 0x0b, ping = 0x0c,
                  display_grid = 0x0d, vrr_policy = 0x0e, touch = 0x0f, auth = 0x10, parallelism = 0x11,
                  rotation = 0x12, stream_codec = 0x13;
// Host -> client.
constexpr uint8_t hello_ack = 0x81, cursor = 0x82, relative = 0x83, rumble = 0x84, pong = 0x85, error = 0x86,
                  cursor_clip = 0x87, encode_format = 0x88;
} // namespace msg

constexpr uint16_t protocol_version = 5;

// Little-endian message building.
struct Writer {
	std::vector<uint8_t> d;
	void u8(uint8_t v) { d.push_back(v); }
	void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
	void i16(int16_t v) { u16(uint16_t(v)); }
	void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
	void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
	void f32(float v) { uint32_t b; memcpy_(&b, &v); u32(b); }
	void f64(double v) { uint64_t b; memcpy_(&b, &v); u64(b); }
	void bytes(const void *p, size_t n) { auto *b = static_cast<const uint8_t *>(p); d.insert(d.end(), b, b + n); }
private:
	template <typename T, typename U> static void memcpy_(T *a, const U *b) { __builtin_memcpy(a, b, sizeof(T)); }
};

static inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return uint32_t(rd16(p)) | uint32_t(rd16(p + 2)) << 16; }
static inline uint64_t rd64(const uint8_t *p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

// One encoded frame (H.264, HEVC, AV1), whole.
struct Frame {
	uint32_t id = 0;
	bool key = false;
	uint8_t codec = 0;
	int width = 0, height = 0;
	uint64_t slot_ns = 0, encoded_ns = 0; // host clock
	std::vector<uint8_t> bits;
	uint64_t received_ns = 0; // last datagram's kernel arrival (ours)
	uint64_t first_ns = 0;    // first datagram's
	uint64_t overlap_ns = 0;  // sent before the encode finished (host)
};

struct PyroWavePacket {
	uint32_t frame_id;
	int index, count, width, height;
	uint64_t slot_ns, encoded_ns;
	const uint8_t *payload;
	size_t size;
	uint64_t received_ns;
};

struct RiptidePacket {
	uint32_t frame_id;
	int index, count; // count: the frame's packets, known in its last stripe (else 0)
	int width, height;
	uint64_t slot_ns, encoded_ns;
	const uint8_t *payload;
	size_t size;
	uint64_t received_ns;
	bool predicted;
	uint32_t repaired;
	int stripe, stripes, first_rows, stripe_rows, stripe_first, stripe_count, stripe_inter;
};

class ConnectionListener {
public:
	virtual ~ConnectionListener() = default;
	// On the network thread.
	virtual void on_frame(Frame &&) {}
	virtual void on_audio(const uint8_t *, size_t, uint64_t /*pts_ns*/) {}
	virtual void on_pyrowave(const PyroWavePacket &) {}
	virtual void on_riptide(const RiptidePacket &) {}
	// Control messages, also on the network thread.
	virtual void on_hello_ack(uint8_t /*codec*/) {}
	virtual void on_cursor(int /*w*/, int /*h*/, int /*hx*/, int /*hy*/, const uint8_t * /*rgba*/, size_t) {}
	virtual void on_relative(bool) {}
	virtual void on_cursor_clip(int, int, int, int) {}
	virtual void on_rumble(int /*index*/, uint16_t /*strong*/, uint16_t /*weak*/, int /*ms*/) {}
	virtual void on_encode_format(const std::string &) {}
	// Any thread; the session is over (the reason in words).
	virtual void on_closed(const std::string &) {}
};

struct HelloParams {
	int width = 1920, height = 1080;
	uint32_t refresh_mhz = 60000;
	uint32_t kbps = 40000;
	uint8_t codec_mask = 0b111;
	uint8_t preferred = 2;
	bool pieces = true; // AV1 / HEVC frames in pieces (only sent with them)
	bool hdr = false;
};

// This device's identity and the hosts it has met (apple/Identity.swift).
class Identity {
public:
	explicit Identity(const std::string &dir); // dir/device.key, dir/known_hosts
	const uint8_t *seed() const { return seed_; }
	const uint8_t *pub() const { return pub_; }
	// The line for the host's ~/.ssh/authorized_keys.
	std::string authorized_line(const std::string &device) const;
	// Whether `key` is the one `host` had before; the first is remembered.
	bool host_key_matches(const std::string &host, const uint8_t key[32]);
	void forget_hosts();

private:
	std::string dir_;
	uint8_t seed_[32], pub_[32];
	std::mutex lock_;
};

class Connection {
public:
	Connection(Identity &id, ConnectionListener &l) : id_(id), l_(l) {}
	~Connection();
	// Resolves and connects the socket (blocks while the name resolves).
	bool open(const std::string &host, uint16_t port, std::string &err);
	void start(const HelloParams &);
	void stop();

	void send(uint8_t type, const std::vector<uint8_t> &payload = {});
	void motion(double dx, double dy);
	void position(float x, float y);
	void riptide_loss(uint32_t frame_id, int count, const std::vector<std::pair<int, int>> &ranges);
	void keep_awake(bool on);
	void ping(); // also the keepalive, every 500 ms

	// Host clock minus ours, from the fastest ping; false until known.
	bool clock_offset(int64_t &off) const;
	bool to_local(uint64_t host_ns, uint64_t &local) const;
	bool to_host(uint64_t local_ns, uint64_t &host) const;
	uint64_t rtt_ns() const { return rtt_.load(); }
	int max_datagram() const { return link_max_; }

private:
	enum Kind : uint8_t { k_control = 0, k_mouse = 1, k_ack = 2, k_loss = 3 };
	static void nonce(Kind, uint32_t seq, uint8_t out[12]);
	void transmit(const std::vector<uint8_t> &bytes); // under send_lock_
	void send_mouse(const std::vector<uint8_t> &payload); // under send_lock_
	void send_handshake();
	void handshake_reply(const uint8_t *d, size_t n);
	void receive_loop();
	void timer_loop();
	void datagram(const uint8_t *d, size_t n, uint64_t arrived);
	void video_fragment(const uint8_t *d, size_t n, uint64_t arrived);
	void video_chunk(const uint8_t *d, size_t n, uint64_t arrived);
	void control_datagram(uint32_t seq, std::vector<uint8_t> &&payload);
	void parse_control();
	void handle_control(uint8_t type, const uint8_t *b, size_t n);
	void closed(const std::string &why);
	int raw_send(const void *p, size_t n);

	Identity &id_;
	ConnectionListener &l_;
	std::string host_name_;
	int fd_ = -1;
	int link_max_ = 1400;
	uint8_t eph_secret_[32], eph_pub_[32];

	std::mutex send_lock_;
	std::unique_ptr<AesGcm> send_key_; // under send_lock_
	std::unique_ptr<AesGcm> recv_key_; // receive thread only
	uint32_t send_seq_ = 0, mouse_seq_ = 0, loss_seq_ = 0, ack_seq_ = 0;
	// The host's control stream received up to here: acknowledged on each
	// control datagram and every 250 ms, so the host sends a lost one again.
	std::atomic<uint32_t> ack_next_{0};
	void send_ack(); // under send_lock_
	std::vector<std::vector<uint8_t>> unsent_;
	struct Unacked { uint32_t seq; std::vector<uint8_t> d; uint64_t sent; };
	std::vector<Unacked> unacked_;
	double mouse_x_ = 0, mouse_y_ = 0;
	std::vector<uint8_t> last_mouse_;
	bool mouse_repeated_ = true;
	bool awake_ = false;

	std::atomic<bool> running_{false};
	std::atomic<bool> closed_{false};
	std::thread recv_thread_, timer_thread_;
	std::mutex timer_mx_;
	std::condition_variable timer_cv_;

	// Host -> client control stream (network thread).
	uint32_t control_next_ = 0;
	std::map<uint32_t, std::vector<uint8_t>> control_pending_;
	std::vector<uint8_t> control_buf_;

	// Video reassembly (network thread).
	struct Fragments { std::vector<std::vector<uint8_t>> parts; std::vector<bool> have; int got = 0; uint64_t first = 0; };
	std::map<uint32_t, Fragments> fragments_;
	struct Chunked {
		std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts; // index -> (offset, bytes)
		std::vector<bool> have;
		int got = 0, count = 0;
		uint32_t total = 0;
		bool key = false;
		uint8_t codec = 0;
		int width = 0, height = 0;
		uint64_t slot = 0, encoded = 0, overlap = 0, first = 0;
	};
	std::map<uint32_t, Chunked> chunked_;

	mutable std::mutex clock_lock_;
	uint64_t best_rtt_ = UINT64_MAX;
	bool have_offset_ = false;
	int64_t offset_ = 0;
	std::atomic<uint64_t> rtt_{0};
	std::atomic<uint64_t> last_heard_{0};
};

} // namespace gm
