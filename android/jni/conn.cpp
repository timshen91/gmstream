#include "conn.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

namespace gm {

static constexpr int max_control = 1400;      // a control datagram, all in
static constexpr int handshake_size = 129;
static constexpr int tag_size = 16;
static constexpr uint8_t dg_audio = 1, dg_video = 2, dg_control = 3, dg_pyrowave = 4, dg_awake = 5,
                         dg_handshake = 6, dg_mouse = 7, dg_ack = 8, dg_chunk = 9, dg_riptide = 10,
                         dg_loss = 11;
static constexpr int chunk_header = 43, riptide_header = 43;

uint64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- Identity -----------------------------------------------------------------

Identity::Identity(const std::string &dir) : dir_(dir)
{
	std::string file = dir + "/device.key";
	FILE *f = fopen(file.c_str(), "rb");
	bool ok = f && fread(seed_, 1, 32, f) == 32;
	if (f)
		fclose(f);
	if (!ok) {
		random_bytes(seed_, 32);
		int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) {
			if (write(fd, seed_, 32) != 32)
				LOGE("cannot write %s", file.c_str());
			close(fd);
		}
	}
	ed25519_public(seed_, pub_);
}

std::string Identity::authorized_line(const std::string &device) const
{
	std::vector<uint8_t> blob;
	auto be32 = [&](uint32_t v) { for (int i = 3; i >= 0; i--) blob.push_back(uint8_t(v >> (8 * i))); };
	const char *name = "ssh-ed25519";
	be32(uint32_t(strlen(name)));
	blob.insert(blob.end(), name, name + strlen(name));
	be32(32);
	blob.insert(blob.end(), pub_, pub_ + 32);
	return "restrict,command=\"/bin/false\" ssh-ed25519 " + base64(blob.data(), blob.size()) + " gmstream-" + device;
}

static std::string hexs(const uint8_t *p, size_t n)
{
	std::string s;
	char b[3];
	for (size_t i = 0; i < n; i++) {
		snprintf(b, 3, "%02x", p[i]);
		s += b;
	}
	return s;
}

bool Identity::host_key_matches(const std::string &host, const uint8_t key[32])
{
	std::lock_guard<std::mutex> g(lock_);
	std::string file = dir_ + "/known_hosts", want = hexs(key, 32);
	std::ifstream in(file);
	std::string h, k;
	while (in >> h >> k)
		if (h == host)
			return k == want;
	std::ofstream out(file, std::ios::app);
	out << host << " " << want << "\n";
	return true;
}

void Identity::forget_hosts()
{
	std::lock_guard<std::mutex> g(lock_);
	unlink((dir_ + "/known_hosts").c_str());
}

// ---- Connection -------------------------------------------------------------------

Connection::~Connection()
{
	stop();
}

static int link_max_datagram(int fd)
{
	sockaddr_storage local{};
	socklen_t len = sizeof(local);
	if (getsockname(fd, reinterpret_cast<sockaddr *>(&local), &len) != 0)
		return 1400;
	ifaddrs *ifs = nullptr;
	if (getifaddrs(&ifs) != 0)
		return 1400;
	std::string name;
	for (ifaddrs *p = ifs; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != local.ss_family)
			continue;
		bool same = false;
		if (local.ss_family == AF_INET)
			same = reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr.s_addr ==
			       reinterpret_cast<sockaddr_in *>(&local)->sin_addr.s_addr;
		else if (local.ss_family == AF_INET6)
			same = !memcmp(&reinterpret_cast<sockaddr_in6 *>(p->ifa_addr)->sin6_addr,
			               &reinterpret_cast<sockaddr_in6 *>(&local)->sin6_addr, 16);
		if (same) {
			name = p->ifa_name;
			break;
		}
	}
	freeifaddrs(ifs);
	if (name.empty())
		return 1400;
	ifreq ifr{};
	strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFMTU, &ifr) != 0)
		return 1400;
	int headers = local.ss_family == AF_INET6 ? 48 : 28;
	return std::max(1400, std::min(ifr.ifr_mtu - headers, 65507));
}

bool Connection::open(const std::string &host, uint16_t port, std::string &err)
{
	host_name_ = host;
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
	if (rc != 0 || !res) {
		err = "cannot resolve " + host + ": " + gai_strerror(rc);
		return false;
	}
	fd_ = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
	if (fd_ < 0) {
		err = std::string("socket: ") + strerror(errno);
		freeaddrinfo(res);
		return false;
	}
	int size = 8 << 20;
	setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
#ifdef SO_RCVBUFFORCE
	setsockopt(fd_, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)); // refused without the capability
#endif
#ifdef SO_TIMESTAMPNS
	int on = 1;
	setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
#endif
	// Uplink in the video access category (AF41), as the host marks its own.
	int tos = 0x88;
	if (res->ai_family == AF_INET)
		setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
	else
		setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
#ifdef SO_PRIORITY
	int prio = 5;
	setsockopt(fd_, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
#endif
	rc = connect(fd_, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (rc != 0) {
		err = std::string("connect: ") + strerror(errno);
		close(fd_);
		fd_ = -1;
		return false;
	}
	link_max_ = link_max_datagram(fd_);
	random_bytes(eph_secret_, 32);
	x25519_public(eph_secret_, eph_pub_);
	last_heard_ = now_ns();
	return true;
}

void Connection::start(const HelloParams &p)
{
	Writer w;
	w.u16(protocol_version);
	w.u16(uint16_t(p.width));
	w.u16(uint16_t(p.height));
	w.u32(p.refresh_mhz);
	w.u32(p.kbps);
	w.u8(p.codec_mask);
	w.u8(p.preferred);
	bool pieces = p.pieces && (p.preferred == 1 || p.preferred == 2);
	w.u8(uint8_t((pieces ? 4 : 0) | (p.hdr ? 8 : 0)));
	w.u16(uint16_t(link_max_));
	running_ = true;
	send(msg::hello, w.d);
	send_handshake();
	recv_thread_ = std::thread([this] { receive_loop(); });
	timer_thread_ = std::thread([this] { timer_loop(); });
	ping();
}

void Connection::stop()
{
	if (fd_ < 0)
		return;
	{
		std::lock_guard<std::mutex> g(send_lock_);
		running_ = false;
	}
	timer_cv_.notify_all();
	shutdown(fd_, SHUT_RDWR);
	for (std::thread *t : {&recv_thread_, &timer_thread_}) {
		if (!t->joinable())
			continue;
		if (t->get_id() == std::this_thread::get_id())
			t->detach();
		else
			t->join();
	}
	std::lock_guard<std::mutex> g(send_lock_);
	close(fd_);
	fd_ = -1;
}

int Connection::raw_send(const void *p, size_t n)
{
	return int(::send(fd_, p, n, 0));
}

void Connection::nonce(Kind k, uint32_t seq, uint8_t out[12])
{
	memset(out, 0, 12);
	out[0] = k;
	out[8] = uint8_t(seq >> 24);
	out[9] = uint8_t(seq >> 16);
	out[10] = uint8_t(seq >> 8);
	out[11] = uint8_t(seq);
}

void Connection::send(uint8_t type, const std::vector<uint8_t> &payload)
{
	Writer m;
	m.u8(type);
	m.u32(uint32_t(payload.size()));
	m.bytes(payload.data(), payload.size());
	std::lock_guard<std::mutex> g(send_lock_);
	if (!running_)
		return;
	if (!send_key_) {
		unsent_.push_back(std::move(m.d));
		return;
	}
	transmit(m.d);
}

void Connection::transmit(const std::vector<uint8_t> &bytes)
{
	size_t off = 0;
	while (off < bytes.size()) {
		size_t n = std::min(size_t(max_control - 5 - tag_size), bytes.size() - off);
		std::vector<uint8_t> d(5 + n + tag_size);
		d[0] = dg_control;
		d[1] = uint8_t(send_seq_);
		d[2] = uint8_t(send_seq_ >> 8);
		d[3] = uint8_t(send_seq_ >> 16);
		d[4] = uint8_t(send_seq_ >> 24);
		uint8_t iv[12];
		nonce(k_control, send_seq_, iv);
		send_key_->seal(iv, d.data(), 5, bytes.data() + off, n, d.data() + 5);
		if (raw_send(d.data(), d.size()) < 0 && running_)
			LOGW("send: %s", strerror(errno));
		unacked_.push_back({send_seq_, std::move(d), now_ns()});
		send_seq_++;
		off += n;
	}
}

// To the host: `u32 next`, the first of its control datagrams not yet
// received (PROTOCOL.md, "Plain UDP").
void Connection::send_ack()
{
	if (!send_key_ || !running_)
		return;
	uint32_t next = ack_next_.load();
	uint8_t plain[4] = {uint8_t(next), uint8_t(next >> 8), uint8_t(next >> 16), uint8_t(next >> 24)};
	std::vector<uint8_t> d(5 + 4 + tag_size);
	d[0] = dg_ack;
	d[1] = uint8_t(ack_seq_);
	d[2] = uint8_t(ack_seq_ >> 8);
	d[3] = uint8_t(ack_seq_ >> 16);
	d[4] = uint8_t(ack_seq_ >> 24);
	uint8_t iv[12];
	nonce(k_ack, ack_seq_, iv);
	send_key_->seal(iv, d.data(), 5, plain, 4, d.data() + 5);
	ack_seq_++;
	raw_send(d.data(), d.size());
}

void Connection::motion(double dx, double dy)
{
	std::lock_guard<std::mutex> g(send_lock_);
	mouse_x_ += dx;
	mouse_y_ += dy;
	Writer m;
	m.u8(0);
	m.f64(mouse_x_);
	m.f64(mouse_y_);
	send_mouse(m.d);
}

void Connection::position(float x, float y)
{
	std::lock_guard<std::mutex> g(send_lock_);
	Writer m;
	m.u8(1);
	m.f32(x);
	m.f32(y);
	send_mouse(m.d);
}

void Connection::send_mouse(const std::vector<uint8_t> &payload)
{
	if (!running_ || !send_key_)
		return;
	std::vector<uint8_t> d(5 + payload.size() + tag_size);
	d[0] = dg_mouse;
	memcpy(&d[1], &mouse_seq_, 4); // little-endian hosts only (arm64, x86)
	uint8_t iv[12];
	nonce(k_mouse, mouse_seq_, iv);
	mouse_seq_++;
	send_key_->seal(iv, d.data(), 5, payload.data(), payload.size(), d.data() + 5);
	raw_send(d.data(), d.size());
	last_mouse_ = payload;
	mouse_repeated_ = false;
}

void Connection::riptide_loss(uint32_t frame_id, int count, const std::vector<std::pair<int, int>> &ranges)
{
	std::lock_guard<std::mutex> g(send_lock_);
	if (!running_ || !send_key_)
		return;
	Writer m;
	m.u32(frame_id);
	m.u16(uint16_t(std::clamp(count, 0, 65535)));
	size_t n = std::min<size_t>(ranges.size(), 300);
	m.u16(uint16_t(n));
	for (size_t i = 0; i < n; i++) {
		m.u16(uint16_t(std::clamp(ranges[i].first, 0, 65535)));
		m.u16(uint16_t(std::clamp(ranges[i].second, 0, 65535)));
	}
	std::vector<uint8_t> d(5 + m.d.size() + tag_size);
	d[0] = dg_loss;
	memcpy(&d[1], &loss_seq_, 4);
	uint8_t iv[12];
	nonce(k_loss, loss_seq_, iv);
	loss_seq_++;
	send_key_->seal(iv, d.data(), 5, m.d.data(), m.d.size(), d.data() + 5);
	raw_send(d.data(), d.size());
}

void Connection::keep_awake(bool on)
{
	std::lock_guard<std::mutex> g(send_lock_);
	awake_ = on;
}

void Connection::send_handshake()
{
	std::lock_guard<std::mutex> g(send_lock_);
	if (!running_ || send_key_)
		return;
	uint8_t d[handshake_size] = {};
	d[0] = dg_handshake;
	memcpy(d + 1, eph_pub_, 32);
	raw_send(d, sizeof(d));
}

void Connection::handshake_reply(const uint8_t *d, size_t)
{
	if (recv_key_)
		return;
	const uint8_t *theirs = d + 1, *host_key = d + 33, *sig = d + 65;
	uint8_t transcript[32];
	Sha256 h;
	h.update("gmstream-udp-1", 14);
	h.update(eph_pub_, 32);
	h.update(theirs, 32);
	h.update(host_key, 32);
	h.final(transcript);
	std::vector<uint8_t> signed_msg;
	const char *hs = "gmstream host";
	signed_msg.insert(signed_msg.end(), hs, hs + strlen(hs));
	signed_msg.insert(signed_msg.end(), transcript, transcript + 32);
	if (!ed25519_verify(host_key, signed_msg.data(), signed_msg.size(), sig)) {
		LOGW("handshake: the host's signature does not verify");
		return;
	}
	if (!id_.host_key_matches(host_name_, host_key)) {
		closed(host_name_ + "'s key has changed. If that's expected, forget the host in Settings.");
		return;
	}
	uint8_t shared[32], k[32];
	x25519(eph_secret_, theirs, shared);
	hkdf_sha256(transcript, 32, shared, 32, reinterpret_cast<const uint8_t *>("h2c"), 3, k, 32);
	recv_key_ = std::make_unique<AesGcm>(k);
	hkdf_sha256(transcript, 32, shared, 32, reinterpret_cast<const uint8_t *>("c2h"), 3, k, 32);
	auto key = std::make_unique<AesGcm>(k);
	Writer auth;
	auth.u8(msg::auth);
	auth.u32(96);
	auth.bytes(id_.pub(), 32);
	std::vector<uint8_t> mine;
	const char *cs = "gmstream client";
	mine.insert(mine.end(), cs, cs + strlen(cs));
	mine.insert(mine.end(), transcript, transcript + 32);
	uint8_t my_sig[64];
	ed25519_sign(id_.seed(), mine.data(), mine.size(), my_sig);
	auth.bytes(my_sig, 64);
	std::lock_guard<std::mutex> g(send_lock_);
	if (!running_)
		return;
	send_key_ = std::move(key);
	transmit(auth.d);
	for (auto &b : unsent_)
		transmit(b);
	unsent_.clear();
	LOGI("handshake done with %s", host_name_.c_str());
}

void Connection::ping()
{
	send_handshake(); // lost, if nothing has come back
	Writer w;
	w.u64(now_ns() / 1000);
	send(msg::ping, w.d);
	uint64_t last = last_heard_, now = now_ns();
	if (now > last && now - last > 5000000000ull)
		closed("No packets from the host for 5 s");
}

void Connection::closed(const std::string &why)
{
	if (closed_.exchange(true))
		return;
	l_.on_closed(why);
}

bool Connection::clock_offset(int64_t &off) const
{
	std::lock_guard<std::mutex> g(clock_lock_);
	off = offset_;
	return have_offset_;
}

bool Connection::to_local(uint64_t host_ns, uint64_t &local) const
{
	int64_t off;
	if (!clock_offset(off))
		return false;
	local = uint64_t(int64_t(host_ns) - off);
	return true;
}

bool Connection::to_host(uint64_t local_ns, uint64_t &host) const
{
	int64_t off;
	if (!clock_offset(off))
		return false;
	host = uint64_t(int64_t(local_ns) + off);
	return true;
}

// Every few ms: control datagrams not acknowledged within 30 ms go again, the
// mouse's last word once more, and the keep-awake datagram at 60 Hz.
void Connection::timer_loop()
{
	uint64_t next_resend = now_ns(), next_awake = now_ns(), next_ack = now_ns();
	std::unique_lock<std::mutex> lk(timer_mx_);
	while (running_) {
		uint64_t now = now_ns();
		if (now >= next_resend) {
			std::lock_guard<std::mutex> g(send_lock_);
			for (auto &u : unacked_)
				if (now - u.sent > 30000000ull) {
					u.sent = now;
					raw_send(u.d.data(), u.d.size());
				}
			if (!mouse_repeated_ && !last_mouse_.empty()) {
				auto m = last_mouse_;
				send_mouse(m);
				mouse_repeated_ = true;
			}
			next_resend = now + 15000000ull;
		}
		if (now >= next_ack) {
			// Also when nothing arrives: the host's last datagram may be the lost one.
			std::lock_guard<std::mutex> g(send_lock_);
			send_ack();
			next_ack = now + 250000000ull;
		}
		if (now >= next_awake) {
			bool on;
			{
				std::lock_guard<std::mutex> g(send_lock_);
				on = awake_ && running_;
				if (on) {
					uint8_t b = dg_awake;
					raw_send(&b, 1);
				}
			}
			next_awake += 1000000000ull / 60;
			if (next_awake < now)
				next_awake = now + 1000000000ull / 60;
		}
		uint64_t wake = std::min({next_resend, next_awake, next_ack});
		now = now_ns();
		if (wake > now)
			timer_cv_.wait_for(lk, std::chrono::nanoseconds(wake - now));
	}
}

void Connection::receive_loop()
{
#if defined(__linux__)
	constexpr int batch = 16;
	static thread_local std::vector<uint8_t> bufs(size_t(batch) * 65536);
	mmsghdr msgs[batch];
	iovec iov[batch];
	alignas(cmsghdr) uint8_t ctrl[batch][64];
	while (running_) {
		for (int i = 0; i < batch; i++) {
			iov[i] = {bufs.data() + size_t(i) * 65536, 65536};
			msgs[i].msg_hdr = {};
			msgs[i].msg_hdr.msg_iov = &iov[i];
			msgs[i].msg_hdr.msg_iovlen = 1;
			msgs[i].msg_hdr.msg_control = ctrl[i];
			msgs[i].msg_hdr.msg_controllen = sizeof(ctrl[i]);
		}
		int n = recvmmsg(fd_, msgs, batch, MSG_WAITFORONE, nullptr);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			if (running_)
				closed(std::string("Network error: ") + (n < 0 ? strerror(errno) : "closed"));
			return;
		}
		uint64_t now = now_ns();
		last_heard_ = now;
		// Kernel stamps are CLOCK_REALTIME; moved onto the monotonic clock.
		timespec real;
		clock_gettime(CLOCK_REALTIME, &real);
		int64_t real_minus_mono = int64_t(real.tv_sec) * 1000000000ll + real.tv_nsec - int64_t(now);
		for (int i = 0; i < n; i++) {
			uint64_t arrived = now;
			for (cmsghdr *c = CMSG_FIRSTHDR(&msgs[i].msg_hdr); c; c = CMSG_NXTHDR(&msgs[i].msg_hdr, c)) {
				if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
					timespec ts;
					memcpy(&ts, CMSG_DATA(c), sizeof(ts));
					int64_t t = int64_t(ts.tv_sec) * 1000000000ll + ts.tv_nsec - real_minus_mono;
					if (t > 0 && uint64_t(t) <= now)
						arrived = uint64_t(t);
				}
			}
			datagram(static_cast<uint8_t *>(iov[i].iov_base), msgs[i].msg_len, arrived);
		}
	}
#else
	std::vector<uint8_t> buf(65536);
	while (running_) {
		ssize_t n = recv(fd_, buf.data(), buf.size(), 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			if (running_)
				closed(std::string("Network error: ") + (n < 0 ? strerror(errno) : "closed"));
			return;
		}
		last_heard_ = now_ns();
		datagram(buf.data(), size_t(n), last_heard_);
	}
#endif
}

void Connection::datagram(const uint8_t *d, size_t n, uint64_t arrived)
{
	if (n < 1)
		return;
	switch (d[0]) {
	case dg_video:
		if (n > 9)
			video_fragment(d, n, arrived);
		break;
	case dg_chunk:
		if (n > size_t(chunk_header))
			video_chunk(d, n, arrived);
		break;
	case dg_handshake:
		if (n >= size_t(handshake_size))
			handshake_reply(d, n);
		break;
	case dg_control: {
		if (n <= size_t(5 + tag_size) || !recv_key_)
			break;
		uint32_t seq = rd32(d + 1);
		uint8_t iv[12];
		nonce(k_control, seq, iv);
		std::vector<uint8_t> pt(n - 5 - tag_size);
		if (!recv_key_->open(iv, d, 5, d + 5, n - 5, pt.data()))
			break;
		control_datagram(seq, std::move(pt));
		break;
	}
	case dg_ack: {
		if (n != size_t(5 + 4 + tag_size) || !recv_key_)
			break;
		uint32_t seq = rd32(d + 1);
		uint8_t iv[12], next[4];
		nonce(k_ack, seq, iv);
		if (!recv_key_->open(iv, d, 5, d + 5, n - 5, next))
			break;
		uint32_t up_to = rd32(next);
		std::lock_guard<std::mutex> g(send_lock_);
		unacked_.erase(std::remove_if(unacked_.begin(), unacked_.end(),
		                              [&](const Unacked &u) { return int32_t(u.seq - up_to) < 0; }),
		               unacked_.end());
		break;
	}
	case dg_pyrowave:
		if (n > 29) {
			PyroWavePacket p{rd32(d + 1), rd16(d + 5), rd16(d + 7), rd16(d + 9), rd16(d + 11),
			                 rd64(d + 13), rd64(d + 21), d + 29, n - 29, arrived};
			l_.on_pyrowave(p);
		}
		break;
	case dg_riptide:
		if (n > size_t(riptide_header)) {
			RiptidePacket p{};
			p.frame_id = rd32(d + 1);
			p.index = rd16(d + 5);
			p.width = rd16(d + 7);
			p.height = rd16(d + 9);
			p.slot_ns = rd64(d + 11);
			p.encoded_ns = rd64(d + 19);
			p.predicted = d[27] & 1;
			p.repaired = rd32(d + 28);
			p.stripe = d[32];
			p.stripes = std::max(1, int(d[33]));
			p.first_rows = rd16(d + 34);
			p.stripe_rows = rd16(d + 36);
			p.stripe_first = rd16(d + 38);
			p.stripe_count = rd16(d + 40);
			p.stripe_inter = d[42];
			p.count = p.stripe + 1 == p.stripes ? p.stripe_first + p.stripe_count : 0;
			p.payload = d + riptide_header;
			p.size = n - riptide_header;
			p.received_ns = arrived;
			l_.on_riptide(p);
		}
		break;
	case dg_audio:
		if (n > 13)
			l_.on_audio(d + 13, n - 13, rd64(d + 5));
		break;
	default:
		break; // fillers (12) and anything newer
	}
}

void Connection::video_fragment(const uint8_t *d, size_t n, uint64_t arrived)
{
	uint32_t id = rd32(d + 1);
	int index = rd16(d + 5), count = rd16(d + 7);
	auto it = fragments_.find(id);
	if (it == fragments_.end()) {
		if (count <= 0)
			return;
		Fragments f;
		f.parts.resize(size_t(count));
		f.have.assign(size_t(count), false);
		f.first = arrived;
		it = fragments_.emplace(id, std::move(f)).first;
		while (fragments_.size() > 16)
			fragments_.erase(fragments_.begin());
		it = fragments_.find(id);
		if (it == fragments_.end())
			return;
	}
	Fragments &f = it->second;
	if (index >= int(f.parts.size()) || f.have[size_t(index)])
		return;
	f.parts[size_t(index)].assign(d + 9, d + n);
	f.have[size_t(index)] = true;
	if (++f.got < int(f.parts.size()))
		return;
	std::vector<uint8_t> data;
	for (auto &p : f.parts)
		data.insert(data.end(), p.begin(), p.end());
	uint64_t first = f.first;
	fragments_.erase(it);
	if (data.size() < 32)
		return;
	Frame fr;
	fr.id = rd32(&data[0]);
	fr.key = data[4] & 1;
	fr.codec = data[5];
	fr.width = rd16(&data[6]);
	fr.height = rd16(&data[8]);
	fr.slot_ns = rd64(&data[12]);
	fr.encoded_ns = rd64(&data[20]);
	fr.bits.assign(data.begin() + 32, data.end());
	fr.received_ns = arrived;
	fr.first_ns = first;
	l_.on_frame(std::move(fr));
}

void Connection::video_chunk(const uint8_t *d, size_t n, uint64_t arrived)
{
	uint32_t id = rd32(d + 1);
	auto it = chunked_.find(id);
	if (it == chunked_.end()) {
		Chunked c;
		c.first = arrived;
		chunked_.emplace(id, std::move(c));
		while (chunked_.size() > 4)
			chunked_.erase(chunked_.begin());
		it = chunked_.find(id);
		if (it == chunked_.end())
			return;
	}
	Chunked &f = it->second;
	uint8_t flags = d[17];
	f.codec = d[18];
	f.width = rd16(d + 19);
	f.height = rd16(d + 21);
	f.slot = rd64(d + 23);
	if (flags & 2) {
		f.key = flags & 1;
		f.count = rd16(d + 7);
		f.total = rd32(d + 13);
		f.encoded = rd64(d + 31);
		f.overlap = rd32(d + 39);
	}
	size_t index = rd16(d + 5);
	if (index >= f.parts.size()) {
		f.parts.resize(index + 1);
		f.have.resize(index + 1, false);
	}
	if (!f.have[index]) {
		f.parts[index] = {rd32(d + 9), std::vector<uint8_t>(d + chunk_header, d + n)};
		f.have[index] = true;
		f.got++;
	}
	if (f.count == 0 || f.got < f.count || f.total == 0)
		return;
	Frame fr;
	fr.bits.assign(f.total, 0);
	for (size_t i = 0; i < f.parts.size(); i++) {
		if (!f.have[i])
			continue;
		auto &p = f.parts[i];
		if (p.first >= f.total)
			continue;
		size_t m = std::min<size_t>(p.second.size(), f.total - p.first);
		memcpy(fr.bits.data() + p.first, p.second.data(), m);
	}
	fr.id = id;
	fr.key = f.key;
	fr.codec = f.codec;
	fr.width = f.width;
	fr.height = f.height;
	fr.slot_ns = f.slot;
	fr.encoded_ns = f.encoded;
	fr.received_ns = arrived;
	fr.first_ns = f.first;
	fr.overlap_ns = f.overlap;
	chunked_.erase(it);
	l_.on_frame(std::move(fr));
}

void Connection::control_datagram(uint32_t seq, std::vector<uint8_t> &&payload)
{
	if (seq == control_next_) {
		control_buf_.insert(control_buf_.end(), payload.begin(), payload.end());
		control_next_++;
		for (auto it = control_pending_.find(control_next_); it != control_pending_.end();
		     it = control_pending_.find(control_next_)) {
			control_buf_.insert(control_buf_.end(), it->second.begin(), it->second.end());
			control_pending_.erase(it);
			control_next_++;
		}
		parse_control();
	} else if (int32_t(seq - control_next_) > 0) {
		control_pending_[seq] = std::move(payload);
		if (control_pending_.size() > 1024)
			closed("The control stream stalled");
	}
	// Each one says how far we have it (the host resends a gap; a duplicate
	// is acknowledged too, in case our last acknowledgement was the one lost).
	ack_next_ = control_next_;
	std::lock_guard<std::mutex> g(send_lock_);
	send_ack();
}

void Connection::parse_control()
{
	size_t off = 0;
	while (control_buf_.size() - off >= 5) {
		uint32_t len = rd32(&control_buf_[off + 1]);
		if (control_buf_.size() - off < 5 + size_t(len))
			break;
		handle_control(control_buf_[off], control_buf_.data() + off + 5, len);
		off += 5 + len;
	}
	control_buf_.erase(control_buf_.begin(), control_buf_.begin() + long(off));
}

void Connection::handle_control(uint8_t type, const uint8_t *b, size_t n)
{
	switch (type) {
	case msg::pong:
		if (n >= 16) {
			uint64_t now = now_ns(), t0 = rd64(b) * 1000, host = rd64(b + 8);
			uint64_t rtt = now - t0;
			std::lock_guard<std::mutex> g(clock_lock_);
			if (!have_offset_ || rtt <= best_rtt_ + best_rtt_ / 5) {
				best_rtt_ = std::min(best_rtt_, rtt);
				offset_ = int64_t(host) - int64_t(t0 + rtt / 2);
				have_offset_ = true;
			}
			rtt_ = rtt;
		}
		break;
	case msg::hello_ack:
		if (n >= 1)
			l_.on_hello_ack(b[0]);
		break;
	case msg::cursor:
		if (n >= 8)
			l_.on_cursor(rd16(b), rd16(b + 2), int16_t(rd16(b + 4)), int16_t(rd16(b + 6)), b + 8, n - 8);
		break;
	case msg::relative:
		if (n >= 1)
			l_.on_relative(b[0] != 0);
		break;
	case msg::cursor_clip:
		if (n >= 8)
			l_.on_cursor_clip(rd16(b), rd16(b + 2), rd16(b + 4), rd16(b + 6));
		break;
	case msg::rumble:
		if (n >= 7)
			l_.on_rumble(b[0], rd16(b + 1), rd16(b + 3), rd16(b + 5));
		break;
	case msg::encode_format:
		l_.on_encode_format(std::string(reinterpret_cast<const char *>(b), n));
		break;
	case msg::error:
		closed(std::string(reinterpret_cast<const char *>(b), n));
		break;
	default:
		break;
	}
}

} // namespace gm
