// The handshake's and the control stream's cryptography (PROTOCOL.md, "Plain
// UDP"): SHA-256, HKDF-SHA256 and AES-256-GCM here, X25519 and Ed25519 from
// Monocypher. Only control, mouse, acknowledgement and loss-report datagrams
// are sealed, a few hundred a second at most, so plain portable code is enough.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gm {

struct Sha256 {
	uint32_t h[8];
	uint8_t buf[64];
	uint64_t len = 0;
	size_t fill = 0;
	Sha256();
	void update(const void *data, size_t n);
	void final(uint8_t out[32]);
};

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]);
// HKDF-SHA256 (RFC 5869), up to 32 bytes of output.
void hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

class AesGcm {
public:
	explicit AesGcm(const uint8_t key[32]);
	// out = ciphertext || 16-byte tag.
	void seal(const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
	          const uint8_t *pt, size_t n, uint8_t *out) const;
	// in = ciphertext || tag; false if it does not open.
	bool open(const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
	          const uint8_t *in, size_t n, uint8_t *out) const;

private:
	uint32_t rk[60];
	uint8_t hkey[16];
	void block(const uint8_t in[16], uint8_t out[16]) const;
	void ghash(const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t n, uint8_t out[16]) const;
	void ctr(const uint8_t nonce[12], const uint8_t *in, size_t n, uint8_t *out) const;
};

// Ed25519 identity: a 32-byte seed.
void ed25519_public(const uint8_t seed[32], uint8_t pub[32]);
void ed25519_sign(const uint8_t seed[32], const uint8_t *msg, size_t n, uint8_t sig[64]);
bool ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64]);
void x25519_public(const uint8_t secret[32], uint8_t pub[32]);
void x25519(const uint8_t secret[32], const uint8_t theirs[32], uint8_t shared[32]);
void random_bytes(uint8_t *out, size_t n);

std::string base64(const uint8_t *data, size_t n);

} // namespace gm
