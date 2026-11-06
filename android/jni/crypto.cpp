#include "crypto.h"

#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/random.h>
#endif

extern "C" {
#include "third_party/monocypher/monocypher.h"
#include "third_party/monocypher/monocypher-ed25519.h"
}

namespace gm {

// ---- SHA-256 ----------------------------------------------------------------

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256()
{
	static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(h, init, sizeof(h));
}

static void sha256_block(uint32_t h[8], const uint8_t p[64])
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
		uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;
		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void Sha256::update(const void *data, size_t n)
{
	auto *p = static_cast<const uint8_t *>(data);
	len += n;
	while (n > 0) {
		size_t take = 64 - fill < n ? 64 - fill : n;
		memcpy(buf + fill, p, take);
		fill += take;
		p += take;
		n -= take;
		if (fill == 64) {
			sha256_block(h, buf);
			fill = 0;
		}
	}
}

void Sha256::final(uint8_t out[32])
{
	uint64_t bits = len * 8;
	uint8_t pad = 0x80;
	update(&pad, 1);
	uint8_t zero = 0;
	while (fill != 56)
		update(&zero, 1);
	uint8_t l[8];
	for (int i = 0; i < 8; i++)
		l[i] = uint8_t(bits >> (56 - 8 * i));
	update(l, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = uint8_t(h[i] >> 24);
		out[4 * i + 1] = uint8_t(h[i] >> 16);
		out[4 * i + 2] = uint8_t(h[i] >> 8);
		out[4 * i + 3] = uint8_t(h[i]);
	}
}

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
	uint8_t k[64] = {};
	if (key_len > 64) {
		Sha256 s;
		s.update(key, key_len);
		s.final(k);
	} else {
		memcpy(k, key, key_len);
	}
	uint8_t ipad[64], opad[64];
	for (int i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	uint8_t inner[32];
	Sha256 a;
	a.update(ipad, 64);
	a.update(msg, msg_len);
	a.final(inner);
	Sha256 b;
	b.update(opad, 64);
	b.update(inner, 32);
	b.final(out);
}

void hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
	uint8_t prk[32];
	hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
	// T(1) = HMAC(PRK, info || 0x01); 32 bytes is all this protocol needs.
	std::string m(reinterpret_cast<const char *>(info), info_len);
	m.push_back('\x01');
	uint8_t t[32];
	hmac_sha256(prk, 32, reinterpret_cast<const uint8_t *>(m.data()), m.size(), t);
	memcpy(out, t, out_len < 32 ? out_len : 32);
}

// ---- AES-256 ----------------------------------------------------------------

static const uint8_t SBOX[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static inline uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x >> 7) * 0x1b)); }

AesGcm::AesGcm(const uint8_t key[32])
{
	// Key expansion, Nk = 8, 15 round keys of 4 words (big-endian words).
	for (int i = 0; i < 8; i++)
		rk[i] = uint32_t(key[4 * i]) << 24 | uint32_t(key[4 * i + 1]) << 16 | uint32_t(key[4 * i + 2]) << 8 | key[4 * i + 3];
	uint8_t rcon = 1;
	for (int i = 8; i < 60; i++) {
		uint32_t t = rk[i - 1];
		if (i % 8 == 0) {
			t = (t << 8) | (t >> 24);
			t = uint32_t(SBOX[t >> 24]) << 24 | uint32_t(SBOX[(t >> 16) & 0xff]) << 16 |
			    uint32_t(SBOX[(t >> 8) & 0xff]) << 8 | SBOX[t & 0xff];
			t ^= uint32_t(rcon) << 24;
			rcon = xtime(rcon);
		} else if (i % 8 == 4) {
			t = uint32_t(SBOX[t >> 24]) << 24 | uint32_t(SBOX[(t >> 16) & 0xff]) << 16 |
			    uint32_t(SBOX[(t >> 8) & 0xff]) << 8 | SBOX[t & 0xff];
		}
		rk[i] = rk[i - 8] ^ t;
	}
	uint8_t zero[16] = {};
	block(zero, hkey);
}

void AesGcm::block(const uint8_t in[16], uint8_t out[16]) const
{
	uint8_t s[16];
	auto add = [&](int round) {
		for (int c = 0; c < 4; c++) {
			uint32_t w = rk[round * 4 + c];
			s[4 * c] ^= uint8_t(w >> 24);
			s[4 * c + 1] ^= uint8_t(w >> 16);
			s[4 * c + 2] ^= uint8_t(w >> 8);
			s[4 * c + 3] ^= uint8_t(w);
		}
	};
	memcpy(s, in, 16);
	add(0);
	for (int round = 1; round <= 14; round++) {
		for (int i = 0; i < 16; i++)
			s[i] = SBOX[s[i]];
		// ShiftRows: state is column-major (s[4c + r]).
		uint8_t t[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				t[4 * c + r] = s[4 * ((c + r) % 4) + r];
		memcpy(s, t, 16);
		if (round != 14) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = s + 4 * c;
				uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				uint8_t all = a0 ^ a1 ^ a2 ^ a3;
				col[0] ^= all ^ xtime(a0 ^ a1);
				col[1] ^= all ^ xtime(a1 ^ a2);
				col[2] ^= all ^ xtime(a2 ^ a3);
				col[3] ^= all ^ xtime(a3 ^ a0);
			}
		}
		add(round);
	}
	memcpy(out, s, 16);
}

// X *= H in GF(2^128), GCM's bit order.
static void gf_mul(uint8_t x[16], const uint8_t h[16])
{
	uint8_t z[16] = {}, v[16];
	memcpy(v, h, 16);
	for (int i = 0; i < 128; i++) {
		if (x[i / 8] & (0x80 >> (i % 8)))
			for (int j = 0; j < 16; j++)
				z[j] ^= v[j];
		bool lsb = v[15] & 1;
		for (int j = 15; j > 0; j--)
			v[j] = uint8_t((v[j] >> 1) | (v[j - 1] << 7));
		v[0] >>= 1;
		if (lsb)
			v[0] ^= 0xe1;
	}
	memcpy(x, z, 16);
}

void AesGcm::ghash(const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t n, uint8_t out[16]) const
{
	uint8_t y[16] = {};
	auto absorb = [&](const uint8_t *p, size_t len) {
		while (len > 0) {
			size_t take = len < 16 ? len : 16;
			for (size_t i = 0; i < take; i++)
				y[i] ^= p[i];
			gf_mul(y, hkey);
			p += take;
			len -= take;
		}
	};
	absorb(aad, aad_len);
	absorb(ct, n);
	uint8_t lens[16];
	uint64_t a = uint64_t(aad_len) * 8, c = uint64_t(n) * 8;
	for (int i = 0; i < 8; i++) {
		lens[i] = uint8_t(a >> (56 - 8 * i));
		lens[8 + i] = uint8_t(c >> (56 - 8 * i));
	}
	absorb(lens, 16);
	memcpy(out, y, 16);
}

void AesGcm::ctr(const uint8_t nonce[12], const uint8_t *in, size_t n, uint8_t *out) const
{
	uint8_t cb[16], ks[16];
	memcpy(cb, nonce, 12);
	uint32_t counter = 2;
	for (size_t off = 0; off < n; off += 16) {
		cb[12] = uint8_t(counter >> 24);
		cb[13] = uint8_t(counter >> 16);
		cb[14] = uint8_t(counter >> 8);
		cb[15] = uint8_t(counter);
		counter++;
		block(cb, ks);
		size_t take = n - off < 16 ? n - off : 16;
		for (size_t i = 0; i < take; i++)
			out[off + i] = in[off + i] ^ ks[i];
	}
}

void AesGcm::seal(const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                  const uint8_t *pt, size_t n, uint8_t *out) const
{
	ctr(nonce, pt, n, out);
	uint8_t s[16], j0[16], e[16];
	ghash(aad, aad_len, out, n, s);
	memcpy(j0, nonce, 12);
	j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
	block(j0, e);
	for (int i = 0; i < 16; i++)
		out[n + i] = s[i] ^ e[i];
}

bool AesGcm::open(const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                  const uint8_t *in, size_t n, uint8_t *out) const
{
	if (n < 16)
		return false;
	size_t len = n - 16;
	uint8_t s[16], j0[16], e[16];
	ghash(aad, aad_len, in, len, s);
	memcpy(j0, nonce, 12);
	j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
	block(j0, e);
	uint8_t diff = 0;
	for (int i = 0; i < 16; i++)
		diff |= uint8_t(s[i] ^ e[i] ^ in[len + i]);
	if (diff)
		return false;
	ctr(nonce, in, len, out);
	return true;
}

// ---- Curve25519 (Monocypher) -------------------------------------------------

void ed25519_public(const uint8_t seed[32], uint8_t pub[32])
{
	uint8_t s[32], sk[64];
	memcpy(s, seed, 32);
	crypto_ed25519_key_pair(sk, pub, s); // wipes s
	crypto_wipe(sk, sizeof(sk));
}

void ed25519_sign(const uint8_t seed[32], const uint8_t *msg, size_t n, uint8_t sig[64])
{
	uint8_t s[32], sk[64], pub[32];
	memcpy(s, seed, 32);
	crypto_ed25519_key_pair(sk, pub, s);
	crypto_ed25519_sign(sig, sk, msg, n);
	crypto_wipe(sk, sizeof(sk));
}

bool ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64])
{
	return crypto_ed25519_check(sig, pub, msg, n) == 0;
}

void x25519_public(const uint8_t secret[32], uint8_t pub[32]) { crypto_x25519_public_key(pub, secret); }
void x25519(const uint8_t secret[32], const uint8_t theirs[32], uint8_t shared[32]) { crypto_x25519(shared, secret, theirs); }

void random_bytes(uint8_t *out, size_t n)
{
#if defined(__ANDROID__) || defined(__linux__)
	while (n > 0) {
		ssize_t got = getrandom(out, n, 0);
		if (got <= 0)
			continue;
		out += got;
		n -= size_t(got);
	}
#else
	arc4random_buf(out, n);
#endif
}

std::string base64(const uint8_t *d, size_t n)
{
	static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string s;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
		s += t[v >> 18];
		s += t[(v >> 12) & 63];
		s += i + 1 < n ? t[(v >> 6) & 63] : '=';
		s += i + 2 < n ? t[v & 63] : '=';
	}
	return s;
}

} // namespace gm
