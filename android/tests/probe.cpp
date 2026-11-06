// A headless client from the Android core, for the build machine: connects,
// authenticates, and counts what arrives (tests/run.sh builds it as probe).
//   probe <dir with device.key> <host> [port] [codec] [seconds]
#include "../jni/conn.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

struct Count : gm::ConnectionListener {
	std::atomic<int> frames{0}, keys{0}, audio{0}, rip{0};
	std::atomic<uint64_t> bytes{0};
	std::atomic<int> codec{-1};
	void on_frame(gm::Frame &&f) override { frames++; keys += f.key; bytes += f.bits.size(); }
	void on_audio(const uint8_t *, size_t, uint64_t) override { audio++; }
	void on_riptide(const gm::RiptidePacket &p) override { rip++; bytes += p.size; }
	void on_hello_ack(uint8_t c) override { codec = c; printf("hello ack: codec %d\n", c); }
	void on_encode_format(const std::string &s) override { printf("host in: %s\n", s.c_str()); }
	void on_closed(const std::string &s) override { printf("closed: %s\n", s.c_str()); exit(1); }
};

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: probe dir host [port] [codec] [seconds]\n"); return 2; }
	gm::Identity id(argv[1]);
	printf("%s\n", id.authorized_line("probe").c_str());
	Count c;
	gm::Connection conn(id, c);
	std::string err;
	if (!conn.open(argv[2], argc > 3 ? atoi(argv[3]) : 8444, err)) { printf("%s\n", err.c_str()); return 1; }
	gm::HelloParams p;
	p.preferred = argc > 4 ? uint8_t(atoi(argv[4])) : 2;
	p.codec_mask = 0b10111;
	int secs = argc > 5 ? atoi(argv[5]) : 5;
	conn.start(p);
	conn.keep_awake(true);
	for (int s = 0; s < secs * 2; s++) {
		usleep(500000);
		conn.ping();
		if (s % 2 == 1) {
			int64_t off = 0;
			bool k = conn.clock_offset(off);
			printf("frames %d keys %d riptide pkts %d audio %d %.1f Mb/s rtt %.2f ms offset %s\n", c.frames.exchange(0),
			       c.keys.exchange(0), c.rip.exchange(0), c.audio.exchange(0), c.bytes.exchange(0) * 8 / 1e6,
			       conn.rtt_ns() / 1e6, k ? "known" : "-");
		}
	}
	conn.stop();
}
