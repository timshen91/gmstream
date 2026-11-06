// Known-answer tests for android/jni/crypto.cpp (run on the build machine:
// android/tests/run.sh).
#include "../jni/crypto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> hex(const char *s)
{
	std::vector<uint8_t> v;
	for (; s[0] && s[1]; s += 2) {
		unsigned b;
		sscanf(s, "%2x", &b);
		v.push_back(uint8_t(b));
	}
	return v;
}
static std::string tohex(const uint8_t *p, size_t n)
{
	std::string s;
	char b[3];
	for (size_t i = 0; i < n; i++) { snprintf(b, 3, "%02x", p[i]); s += b; }
	return s;
}
static int fails = 0;
static void check(const char *what, const std::string &got, const char *want)
{
	bool ok = got == want;
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) { printf("  got  %s\n  want %s\n", got.c_str(), want); fails++; }
}

int main()
{
	uint8_t out[64];
	gm::Sha256 s;
	s.update("abc", 3);
	s.final(out);
	check("sha256 abc", tohex(out, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	std::string million(1000000, 'a');
	gm::Sha256 s2;
	s2.update(million.data(), million.size());
	s2.final(out);
	check("sha256 1M a", tohex(out, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	// RFC 5869 test case 1 (first 32 bytes of OKM).
	auto ikm = hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
	auto salt = hex("000102030405060708090a0b0c");
	auto info = hex("f0f1f2f3f4f5f6f7f8f9");
	gm::hkdf_sha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), out, 32);
	check("hkdf rfc5869 #1", tohex(out, 32), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf");
	// GCM spec test case 16 (AES-256, 96-bit IV, AAD).
	auto key = hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
	auto pt = hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
	auto aad = hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
	auto iv = hex("cafebabefacedbaddecaf888");
	gm::AesGcm g(key.data());
	std::vector<uint8_t> ct(pt.size() + 16);
	g.seal(iv.data(), aad.data(), aad.size(), pt.data(), pt.size(), ct.data());
	check("gcm #16 ct", tohex(ct.data(), pt.size()), "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
	check("gcm #16 tag", tohex(ct.data() + pt.size(), 16), "76fc6ece0f4e1768cddf8853bb2d551b");
	std::vector<uint8_t> back(pt.size());
	bool opened = g.open(iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), back.data());
	check("gcm #16 open", opened && back == pt ? "yes" : "no", "yes");
	ct[3] ^= 1;
	check("gcm #16 tamper", g.open(iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), back.data()) ? "opened" : "refused", "refused");
	// Test case 13: zero key, empty everything.
	auto zk = hex("0000000000000000000000000000000000000000000000000000000000000000");
	auto ziv = hex("000000000000000000000000");
	gm::AesGcm z(zk.data());
	z.seal(ziv.data(), nullptr, 0, nullptr, 0, out);
	check("gcm #13 tag", tohex(out, 16), "530f8afbc74536b9a963b4f1c4cb738b");
	// RFC 8032 test 1 (Ed25519, empty message).
	auto seed = hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
	uint8_t pub[32], sig[64];
	gm::ed25519_public(seed.data(), pub);
	check("ed25519 pub", tohex(pub, 32), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
	gm::ed25519_sign(seed.data(), nullptr, 0, sig);
	check("ed25519 sig", tohex(sig, 64), "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
	check("ed25519 verify", gm::ed25519_verify(pub, nullptr, 0, sig) ? "yes" : "no", "yes");
	// RFC 7748 X25519 Alice/Bob.
	auto a = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	auto bp = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
	gm::x25519_public(a.data(), pub);
	check("x25519 pub", tohex(pub, 32), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
	gm::x25519(a.data(), bp.data(), out);
	check("x25519 shared", tohex(out, 32), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}
