// wburst: what the network does to a frame, apart from any codec.
//
//   host$   ./wburst send [-p PORT] [-w]
//   client$ ./wburst recv HOST [-p PORT] [-b FRAME_BYTES] [-c CHUNKS] [-g CHUNK_GAP_US]
//                     [-O OFFSETS_US] [-S CHUNK_BYTES] [-P CHUNK_DATAGRAM_BYTES]
//                     [-f FPS] [-t SECONDS] [-s DATAGRAM_BYTES] [-d DSCP] [-k PING_HZ]
//                     [-n SVC] [-o FILE]
//
// The receiver asks the sender for frames of FRAME_BYTES at FPS, each cut into
// CHUNKS bursts (a codec handing out stripes): CHUNK_GAP_US apart, or at the
// comma-separated OFFSETS_US after the frame's start, of CHUNK_BYTES each.
// Every burst is one sendmmsg of datagrams marked DSCP, as gmstream's host sends.
// Every datagram carries its send time. The receiver records each one's kernel
// arrival time (SO_TIMESTAMP_MONOTONIC on macOS) and puts both clocks on one
// axis by pinging at PING_HZ (also the uplink keep-awake) and taking, per
// second, the exchange with the shortest round trip; one-way times are
// therefore only as good as the path is symmetric (within half the round
// trip), while comparisons at the same PING_HZ are not affected. It prints, per
// frame: the last datagram's arrival after the first datagram's send
// ("complete"), after the last burst's send ("tail"), the first datagram's
// one-way delay ("head") and the arrivals' spread, p50/p90/p99/max. -o writes
// every datagram (frame, index, count, chunk, sent, arrived, frame start, ns).
// The receiver echoes each frame's last datagram, and the sender logs that loop
// time on its own clock. -n sets the receiver's SO_NET_SERVICE_TYPE (macOS).
// -w (sender): SO_WIFI_STATUS on every datagram. Measured on an MT7925 access
// point: asking for TX status makes the driver serialize and a frame takes
// ~50 ms, so -w measures nothing but itself.
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#define MAGIC 0x57425354u
enum { T_DATA = 1, T_PING = 2, T_PONG = 3, T_START = 4, T_STOP = 6, T_ECHO = 7 };

struct hdr {
	uint8_t type, chunk, nchunks, pad;
	uint32_t frame;
	uint16_t idx, count;
	uint32_t pad2;
	uint64_t tx_ns;       // this datagram's burst, sender clock
	uint64_t frame_ns;    // the frame's scheduled start
};

struct ping {
	uint8_t type, pad[3];
	uint32_t seq;
	uint64_t t1, t2, t3;  // client send, server receive, server send
};

struct start {
	uint8_t type, dscp, chunks, pad;
	uint32_t frame_bytes, gap_us, fps_milli, seconds, payload;
	int32_t offsets_us[32]; // chunk start after frame start; -1: gap_us * c
	uint32_t sizes[32];     // chunk bytes; 0: equal shares
	uint16_t payloads[32];  // chunk datagram size; 0: payload
};

static uint64_t now_ns(void) {
#ifdef __APPLE__
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) mach_timebase_info(&tb);
	return mach_absolute_time() * tb.numer / tb.denom;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}


#ifndef __APPLE__
static int cmp_d0(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
static uint64_t acktime[1 << 16];
// Wi-Fi TX status (SO_WIFI_STATUS): the driver reports each datagram once the radio is
// done with it; record when the last datagram of each frame was reported.
static void drain_status(int fd) {
	for (;;) {
		static uint8_t eb[4096]; char cm[512];
		struct iovec eio = { eb, sizeof eb };
		struct msghdr emh = { .msg_iov = &eio, .msg_iovlen = 1, .msg_control = cm, .msg_controllen = sizeof cm };
		ssize_t m = recvmsg(fd, &emh, MSG_ERRQUEUE | MSG_DONTWAIT);
		if (m < 0) return;
		uint64_t at = now_ns();
		for (ssize_t o = 0; o + (ssize_t)sizeof(struct hdr) <= m; o++) {
			struct hdr h; memcpy(&h, eb + o, sizeof h);
			if (h.pad2 == MAGIC && h.type == T_DATA) {
				if (h.idx == h.count - 1) acktime[h.frame & 0xffff] = at;
				break;
			}
		}
	}
}
static int do_send(int argc, char **argv) {
	int port = 18999, wifi_status = 0;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-p")) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-w")) wifi_status = 1;
	}
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	int zero = 0, big = 8 << 20;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
	struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
	if (bind(fd, (struct sockaddr *)&a, sizeof a)) { perror("bind"); return 1; }
	if (wifi_status) { int one = 1; if (setsockopt(fd, SOL_SOCKET, SO_WIFI_STATUS, &one, sizeof one)) perror("SO_WIFI_STATUS"); }
	static double sendtime[1 << 16]; static uint64_t txat[1 << 16], echo[1 << 16];
	fprintf(stderr, "listening on %d\n", port);
	static uint8_t buf[65536];
	for (;;) {
		struct sockaddr_storage from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n <= 0) continue;
		if (buf[0] == T_PING && n >= (ssize_t)sizeof(struct ping)) {
			struct ping *p = (struct ping *)buf;
			p->t2 = now_ns(); p->type = T_PONG; p->t3 = now_ns();
			sendto(fd, p, sizeof *p, 0, (struct sockaddr *)&from, fl);
			continue;
		}
		if (buf[0] != T_START) continue;
		struct start s; memcpy(&s, buf, sizeof s);
		int tclass = s.dscp << 2;
		setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof tclass);
		setsockopt(fd, IPPROTO_IP, IP_TOS, &tclass, sizeof tclass);
		int chunks = s.chunks ? s.chunks : 1, payload = s.payload ? s.payload : 1400;
		uint32_t size[32], off_us[32];
		uint32_t assigned = 0, unsized = 0;
		for (int c = 0; c < chunks; c++) { assigned += s.sizes[c]; unsized += s.sizes[c] == 0; }
		for (int c = 0; c < chunks; c++) {
			size[c] = s.sizes[c] ? s.sizes[c] : (s.frame_bytes - assigned) / (unsized ? unsized : 1);
			off_us[c] = s.offsets_us[c] >= 0 ? (uint32_t)s.offsets_us[c] : s.gap_us * c;
		}
		uint64_t period = (uint64_t)(1e12 / s.fps_milli);
		uint64_t t0 = now_ns() + 50000000;
		uint32_t frames = (uint32_t)((uint64_t)s.seconds * s.fps_milli / 1000);
		fprintf(stderr, "start: %u B, %d chunks, %u us apart, %.3f fps, %u s, dscp %u, %d B datagrams\n",
		        s.frame_bytes, chunks, s.gap_us, s.fps_milli / 1000.0, s.seconds, s.dscp, payload);
		static uint8_t pkts[2048][1500 + sizeof(struct hdr)];
		int nsend = 0; memset(acktime, 0, sizeof acktime); memset(echo, 0, sizeof echo);
		struct mmsghdr mm[2048]; struct iovec iov[2048];
		int stop = 0;
		for (uint32_t f = 0; f < frames && !stop; f++) {
			uint64_t ft = t0 + f * period;
			uint32_t count = 0;
			for (int c = 0; c < chunks; c++) { int pl = s.payloads[c] ? s.payloads[c] : payload; count += (size[c] + pl - 1) / pl; }
			uint16_t idx = 0;
			for (int c = 0; c < chunks; c++) {
				uint64_t due = ft + off_us[c] * 1000ull;
				// Answer pings while waiting.
				for (;;) {
					int64_t left = (int64_t)(due - now_ns());
					if (left <= 0) break;
					struct pollfd pf = { fd, POLLIN, 0 };
					int ms = left > 1500000 ? (int)(left / 1000000) - 1 : 0;
					int pr = poll(&pf, 1, ms);
					if (pr > 0 && (pf.revents & POLLERR)) { if (wifi_status) drain_status(fd); }
					if (pr <= 0 || !(pf.revents & POLLIN)) continue;
					ssize_t m = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
					if (m <= 0) continue;
					if (buf[0] == T_PING) {
						struct ping *p = (struct ping *)buf;
						p->t2 = now_ns(); p->type = T_PONG; p->t3 = now_ns();
						sendto(fd, p, sizeof *p, 0, (struct sockaddr *)&from, fl);
					} else if (buf[0] == T_STOP) { stop = 1; break; }
					else if (buf[0] == T_ECHO) { struct ping *p = (struct ping *)buf; echo[p->seq & 0xffff] = now_ns(); }
				}
				if (stop) break;
				int pl = s.payloads[c] ? s.payloads[c] : payload;
				int n = (size[c] + pl - 1) / pl;
				uint64_t tx = now_ns();
				for (int i = 0; i < n; i++) {
					struct hdr *h = (struct hdr *)pkts[i];
					*h = (struct hdr){ T_DATA, (uint8_t)c, (uint8_t)chunks, 0, f, idx++, (uint16_t)count, MAGIC, tx, ft };
					int len = i == n - 1 ? (int)(size[c] - (uint32_t)pl * i) : pl;
					iov[i] = (struct iovec){ pkts[i], (size_t)len < sizeof *h ? sizeof *h : (size_t)len };
					mm[i] = (struct mmsghdr){ .msg_hdr = { .msg_name = &from, .msg_namelen = fl, .msg_iov = &iov[i], .msg_iovlen = 1 } };
				}
				for (int sent = 0; sent < n;) {
					int r = sendmmsg(fd, mm + sent, n - sent, 0);
					if (r < 0) { if (errno == EAGAIN || errno == ENOBUFS) continue; perror("sendmmsg"); break; }
					sent += r;
				}
				sendtime[nsend++ & 0xffff] = (now_ns() - tx) / 1e3;
				if (c == chunks - 1) txat[f & 0xffff] = tx;
				if (wifi_status) drain_status(fd);
			}
		}
		{
			int m = nsend < 65536 ? nsend : 65536; qsort(sendtime, m, sizeof(double), cmp_d0);
			fprintf(stderr, "sendmmsg per burst: p50 %.0f p99 %.0f max %.0f us\n", sendtime[m / 2], sendtime[(int)(m * .99)], sendtime[m - 1]);
			{
				static double ed[65536]; int k = 0;
				for (uint32_t f = s.fps_milli / 1000; f < frames && f < 65536; f++) if (echo[f]) ed[k++] = (echo[f] - txat[f]) / 1e3;
				qsort(ed, k, sizeof(double), cmp_d0);
				if (k) fprintf(stderr, "LOOP last burst sent -> echo of its last datagram back: %d frames, p50 %.0f p90 %.0f p99 %.0f us\n", k, ed[k / 2], ed[k * 9 / 10], ed[(int)(k * .99)]);
			}
			if (wifi_status) {
				static double ad[65536]; int k = 0;
				for (uint32_t f = 0; f < frames && f < 65536; f++) if (acktime[f]) ad[k++] = (acktime[f] - txat[f]) / 1e3;
				qsort(ad, k, sizeof(double), cmp_d0);
				if (k) fprintf(stderr, "last datagram's tx status after its burst's send: %d frames, p50 %.0f p90 %.0f p99 %.0f us\n", k, ad[k / 2], ad[k * 9 / 10], ad[(int)(k * .99)]);
			}
		}
		fprintf(stderr, "done\n");
	}
}
#endif

struct rec { uint32_t frame; uint16_t idx, count; uint8_t chunk; uint64_t tx, rx, frame_ns; };
struct pong { uint64_t t1, t2, t3, t4; };

static int cmp_d(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
static void stat(const char *name, double *v, int n) {
	qsort(v, n, sizeof *v, cmp_d);
	if (n == 0) { printf("  %-9s -\n", name); return; }
	printf("  %-9s p50 %6.3f  p90 %6.3f  p99 %6.3f  max %6.3f ms\n", name, v[n / 2], v[n * 9 / 10], v[(int)(n * 0.99)], v[n - 1]);
}

static int do_recv(int argc, char **argv) {
	const char *host = argv[2], *port = "18999", *out = NULL;
	struct start s = { .type = T_START, .dscp = 34, .chunks = 1, .frame_bytes = 104000, .gap_us = 0,
	                   .fps_milli = 120000, .seconds = 10, .payload = 1400 };
	for (int c = 0; c < 32; c++) s.offsets_us[c] = -1;
	int ping_hz = 60, svc = -1;
	for (int i = 3; i < argc; i++) {
		const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : "0";
		if (!strcmp(o, "-p")) port = v;
		else if (!strcmp(o, "-b")) s.frame_bytes = atoi(v);
		else if (!strcmp(o, "-c")) s.chunks = atoi(v);
		else if (!strcmp(o, "-g")) s.gap_us = atoi(v);
		else if (!strcmp(o, "-f")) s.fps_milli = (uint32_t)(atof(v) * 1000);
		else if (!strcmp(o, "-t")) s.seconds = atoi(v);
		else if (!strcmp(o, "-s")) s.payload = atoi(v);
		else if (!strcmp(o, "-d")) s.dscp = atoi(v);
		else if (!strcmp(o, "-k")) ping_hz = atoi(v);
		else if (!strcmp(o, "-n")) svc = atoi(v);
		else if (!strcmp(o, "-o")) out = v;
		else if (!strcmp(o, "-O")) { // comma list of chunk offsets, us
			char *d = strdup(v), *t; int c = 0;
			for (t = strtok(d, ","); t && c < 32; t = strtok(NULL, ",")) s.offsets_us[c++] = atoi(t);
		} else if (!strcmp(o, "-P")) { // comma list of chunk datagram sizes
			char *d = strdup(v), *t; int c = 0;
			for (t = strtok(d, ","); t && c < 32; t = strtok(NULL, ",")) s.payloads[c++] = atoi(t);
		} else if (!strcmp(o, "-S")) { // comma list of chunk sizes, bytes
			char *d = strdup(v), *t; int c = 0;
			for (t = strtok(d, ","); t && c < 32; t = strtok(NULL, ",")) s.sizes[c++] = atoi(t);
		} else continue;
		i++;
	}
	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *ai;
	if (getaddrinfo(host, port, &hints, &ai)) { fprintf(stderr, "resolve\n"); return 1; }
	int fd = socket(ai->ai_family, SOCK_DGRAM, 0);
	int big = 8 << 20, on = 1;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
#ifdef __APPLE__
	if (svc < 0) svc = NET_SERVICE_TYPE_VI;
	if (svc > 0) setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &svc, sizeof svc);
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &on, sizeof on);
#else
	(void)on; (void)svc;
#endif
	connect(fd, ai->ai_addr, ai->ai_addrlen);
	send(fd, &s, sizeof s, 0);
	uint64_t begin = now_ns(), end = begin + (s.seconds + 1) * 1000000000ull + 200000000;
	size_t cap = 1 << 22, nr = 0, np = 0;
	struct rec *r = malloc(cap * sizeof *r);
	struct pong *pg = malloc(100000 * sizeof *pg);
	uint64_t next_ping = begin; uint32_t seq = 0;
	static uint8_t buf[65536];
	uint64_t ping_period = ping_hz > 0 ? 1000000000ull / ping_hz : ~0ull;
#ifdef __APPLE__
	mach_timebase_info_data_t tb; mach_timebase_info(&tb);
#endif
	while (now_ns() < end) {
		uint64_t n = now_ns();
		if (n >= next_ping) {
			struct ping p = { T_PING, {0}, seq++, now_ns(), 0, 0 };
			send(fd, &p, sizeof p, 0);
			next_ping += ping_period;
			if (next_ping < n) next_ping = n + ping_period;
		}
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, 1) <= 0) continue;
		for (;;) {
			struct iovec io = { buf, sizeof buf };
			char cm[256];
			struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm, .msg_controllen = sizeof cm };
			ssize_t m = recvmsg(fd, &mh, MSG_DONTWAIT);
			if (m <= 0) break;
			uint64_t rx = now_ns();
#ifdef __APPLE__
			for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
				if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP_MONOTONIC) {
					uint64_t t; memcpy(&t, CMSG_DATA(c), 8); rx = t * tb.numer / tb.denom;
				}
#endif
			if (buf[0] == T_DATA && nr < cap) {
				struct hdr *h = (struct hdr *)buf;
				r[nr++] = (struct rec){ h->frame, h->idx, h->count, h->chunk, h->tx_ns, rx, h->frame_ns };
				if (h->idx == h->count - 1) { struct ping e = { T_ECHO, {0}, h->frame, 0, 0, 0 }; send(fd, &e, sizeof e, 0); }
			} else if (buf[0] == T_PONG && np < 100000) {
				struct ping *p = (struct ping *)buf;
				pg[np++] = (struct pong){ p->t1, p->t2, p->t3, rx };
			}
		}
	}
	uint8_t stop = T_STOP; send(fd, &stop, 1, 0);
	// Offset (sender clock - receiver clock) per second from the shortest round trip, then
	// interpolated, so drift between the clocks does not leak in.
	int secs = s.seconds + 3; double *off_t = calloc(secs, sizeof(double)), *off_v = calloc(secs, sizeof(double));
	double *best = calloc(secs, sizeof(double)); for (int i = 0; i < secs; i++) best[i] = 1e18;
	double rtt_min = 1e18;
	for (size_t i = 0; i < np; i++) {
		int b = (int)((pg[i].t1 - begin) / 1000000000ull); if (b >= secs) continue;
		double rtt = (double)(pg[i].t4 - pg[i].t1) - (double)(pg[i].t3 - pg[i].t2);
		if (rtt < rtt_min) rtt_min = rtt;
		if (rtt < best[b]) { best[b] = rtt; off_t[b] = (pg[i].t1 + pg[i].t4) / 2.0;
			off_v[b] = (pg[i].t2 + pg[i].t3) / 2.0 - (pg[i].t1 + pg[i].t4) / 2.0; }
	}
	int nb = 0; for (int i = 0; i < secs; i++) if (best[i] < 1e17) { off_t[nb] = off_t[i]; off_v[nb] = off_v[i]; best[nb] = best[i]; nb++; }
	if (nb == 0) { fprintf(stderr, "no pongs\n"); return 1; }
	#define OFF(t) ({ double _t = (t); int _j = 0; while (_j + 1 < nb && off_t[_j + 1] < _t) _j++; \
		_j + 1 < nb && _t > off_t[_j] ? off_v[_j] + (off_v[_j + 1] - off_v[_j]) * (_t - off_t[_j]) / (off_t[_j + 1] - off_t[_j]) : off_v[_j]; })
	// Per frame.
	uint32_t nf = 0; for (size_t i = 0; i < nr; i++) if (r[i].frame + 1 > nf) nf = r[i].frame + 1;
	double *complete = malloc(nf * sizeof(double)), *tail = malloc(nf * sizeof(double)), *head = malloc(nf * sizeof(double)), *spread = malloc(nf * sizeof(double));
	uint64_t *ftx0 = calloc(nf, 8), *ftx1 = calloc(nf, 8), *frx0 = calloc(nf, 8), *frx1 = calloc(nf, 8);
	uint32_t *got = calloc(nf, 4), *cnt = calloc(nf, 4);
	for (size_t i = 0; i < nr; i++) {
		struct rec *x = &r[i]; uint32_t f = x->frame;
		uint64_t rx = x->rx + (int64_t)OFF((double)x->rx);
		if (!got[f] || x->tx < ftx0[f]) ftx0[f] = x->tx;
		if (!got[f] || x->tx > ftx1[f]) ftx1[f] = x->tx;
		if (!got[f] || rx < frx0[f]) frx0[f] = rx;
		if (!got[f] || rx > frx1[f]) frx1[f] = rx;
		got[f]++; cnt[f] = x->count;
	}
	int k = 0, lost = 0, skip = (int)(s.fps_milli / 1000); // first second: settling
	for (uint32_t f = skip; f < nf; f++) {
		if (!got[f]) continue;
		if (got[f] < cnt[f]) { lost++; continue; }
		complete[k] = (frx1[f] - (double)ftx0[f]) / 1e6; tail[k] = (frx1[f] - (double)ftx1[f]) / 1e6;
		head[k] = (frx0[f] - (double)ftx0[f]) / 1e6; spread[k] = (frx1[f] - (double)frx0[f]) / 1e6; k++;
	}
	double txspan = nf > (uint32_t)skip + 1 ? (ftx1[skip + 1] - (double)ftx0[skip + 1]) / 1e6 : 0;
	printf("%u B, %d chunks, gap %u us, %.0f fps, dscp %u, %u B datagrams: %d frames, %d incomplete, rtt min %.3f ms, send span %.3f ms\n",
	       s.frame_bytes, s.chunks, s.gap_us, s.fps_milli / 1000.0, s.dscp, s.payload, k, lost, rtt_min / 1e6, txspan);
	stat("complete", complete, k); stat("tail", tail, k); stat("head", head, k); stat("rx span", spread, k);
	if (out) {
		FILE *o = fopen(out, "w");
		for (size_t i = 0; i < nr; i++)
			fprintf(o, "%u %u %u %u %.0f %.0f %.0f\n", r[i].frame, r[i].idx, r[i].count, r[i].chunk,
			        (double)r[i].tx, r[i].rx + OFF((double)r[i].rx), (double)r[i].frame_ns);
		fclose(o);
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc >= 2 && !strcmp(argv[1], "send")) {
#ifndef __APPLE__
		return do_send(argc, argv);
#endif
	}
	if (argc >= 3 && !strcmp(argv[1], "recv")) return do_recv(argc, argv);
	fprintf(stderr, "usage: wburst send | recv HOST [options]\n");
	return 2;
}
