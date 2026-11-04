//! Who may connect over plain UDP, and the keys for their control stream.
//!
//! A client sends an ephemeral X25519 key. The host answers with its own, its
//! long-term Ed25519 key and a signature over the exchange; the client pins that
//! key the first time it sees it, as SSH does. Each direction gets an AES-256-GCM
//! key from the shared secret. The client's first control message then signs the
//! exchange with its own Ed25519 key, which must be in the host user's
//! ~/.ssh/authorized_keys. The signature covers the fresh keys, so it can't be
//! replayed or moved to another session.
//!
//! The control stream, mouse datagrams and acks are encrypted, which is everything
//! a client sends and what it is told about the mouse. Video and audio are not.

use std::path::Path;

use anyhow::{Context, Result, anyhow, bail};
use ring::aead::{AES_256_GCM, Aad, LessSafeKey, Nonce, UnboundKey};
use ring::agreement::{self, EphemeralPrivateKey, UnparsedPublicKey, X25519};
use ring::rand::SystemRandom;
use ring::signature::{self, Ed25519KeyPair, KeyPair};
use ring::{digest, hkdf};

const CONTEXT: &[u8] = b"gmstream-udp-1";
pub const TAG_SIZE: usize = 16;
/// A handshake datagram from a client: padded to the size of the answer, so
/// the host can't be used to multiply traffic towards a forged address.
pub const HANDSHAKE_SIZE: usize = 129;

/// The host's long-term key, kept in its state directory.
pub struct HostKey(Ed25519KeyPair);

impl HostKey {
    pub fn load(dir: &Path) -> Result<Self> {
        let path = dir.join("host.key");
        let bytes = match std::fs::read(&path) {
            Ok(b) => b,
            Err(_) => {
                let pkcs8 = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).map_err(|_| anyhow!("no randomness"))?;
                write_private(&path, pkcs8.as_ref()).with_context(|| path.display().to_string())?;
                pkcs8.as_ref().to_vec()
            }
        };
        Ok(HostKey(Ed25519KeyPair::from_pkcs8(&bytes).map_err(|_| anyhow!("{}: not a key", path.display()))?))
    }
}

fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let mut f = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(path)?;
    f.write_all(bytes)
}

/// One session's keys.
pub struct Keys {
    send: LessSafeKey,
    recv: LessSafeKey,
    transcript: [u8; 32],
}

/// What a sequence number counts: each has its own numbers under one key, and
/// its own nonces.
#[derive(Clone, Copy)]
pub enum Kind {
    Control = 0,
    Mouse = 1,
    Ack = 2,
    Loss = 3,
}

fn nonce(kind: Kind, seq: u32) -> Nonce {
    let mut n = [0u8; 12];
    n[0] = kind as u8;
    n[8..].copy_from_slice(&seq.to_be_bytes());
    Nonce::assume_unique_for_key(n)
}

impl Keys {
    /// `plain` encrypted, tag appended. A sequence number is used once in each
    /// direction of a session, and the two directions have different keys.
    pub fn seal(&self, kind: Kind, header: &[u8], seq: u32, plain: &[u8]) -> Vec<u8> {
        let mut out = plain.to_vec();
        self.send.seal_in_place_append_tag(nonce(kind, seq), Aad::from(header), &mut out).expect("sealing");
        out
    }

    pub fn open(&self, kind: Kind, header: &[u8], seq: u32, sealed: &[u8]) -> Option<Vec<u8>> {
        let mut out = sealed.to_vec();
        let n = self.recv.open_in_place(nonce(kind, seq), Aad::from(header), &mut out).ok()?.len();
        out.truncate(n);
        Some(out)
    }
}

fn key(prk: &hkdf::Prk, info: &[u8]) -> Result<LessSafeKey, ring::error::Unspecified> {
    let info = [info];
    let okm = prk.expand(&info, &AES_256_GCM)?;
    Ok(LessSafeKey::new(UnboundKey::from(okm)))
}

/// Answers a client's ephemeral key: the reply's body, and the session's keys.
pub fn respond(host: &HostKey, client: &[u8]) -> Result<(Vec<u8>, Keys)> {
    let ours = EphemeralPrivateKey::generate(&X25519, &SystemRandom::new()).map_err(|_| anyhow!("no randomness"))?;
    let ours_public = ours.compute_public_key().map_err(|_| anyhow!("public key"))?;
    let host_public = host.0.public_key().as_ref();
    let mut ctx = digest::Context::new(&digest::SHA256);
    for part in [CONTEXT, client, ours_public.as_ref(), host_public] {
        ctx.update(part);
    }
    let mut transcript = [0u8; 32];
    transcript.copy_from_slice(ctx.finish().as_ref());
    let signed = host.0.sign(&[b"gmstream host".as_slice(), &transcript].concat());
    let (send, recv) = agreement::agree_ephemeral(ours, &UnparsedPublicKey::new(&X25519, client), |shared| {
        let prk = hkdf::Salt::new(hkdf::HKDF_SHA256, &transcript).extract(shared);
        Ok::<_, ring::error::Unspecified>((key(&prk, b"h2c")?, key(&prk, b"c2h")?))
    })
    .map_err(|_| anyhow!("bad client key"))?
    .map_err(|_| anyhow!("key derivation"))?;
    let reply = [ours_public.as_ref(), host_public, signed.as_ref()].concat();
    Ok((reply, Keys { send, recv, transcript }))
}

/// The client's Auth message: its public key and its signature over the
/// exchange. Checks the key is one the host's user has listed.
pub fn verify(keys: &Keys, body: &[u8]) -> Result<()> {
    if body.len() != 96 {
        bail!("bad Auth message");
    }
    let (public, sig) = body.split_at(32);
    signature::UnparsedPublicKey::new(&signature::ED25519, public)
        .verify(&[b"gmstream client".as_slice(), &keys.transcript].concat(), sig)
        .map_err(|_| anyhow!("bad signature"))?;
    let home = std::env::var_os("HOME").context("no HOME")?;
    let path = Path::new(&home).join(".ssh/authorized_keys");
    let listed = std::fs::read_to_string(&path).unwrap_or_default();
    if !lists(&listed, &base64(&ssh_blob(public))) {
        bail!("key {} is not in {}", base64(public), path.display());
    }
    Ok(())
}

/// An OpenSSH public key's binary form.
fn ssh_blob(public: &[u8]) -> Vec<u8> {
    let name = b"ssh-ed25519";
    let mut b = (name.len() as u32).to_be_bytes().to_vec();
    b.extend_from_slice(name);
    b.extend_from_slice(&(public.len() as u32).to_be_bytes());
    b.extend_from_slice(public);
    b
}

fn base64(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in bytes.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            out.push(if i > c.len() { '=' } else { TABLE[(n >> (18 - 6 * i) & 63) as usize] as char });
        }
    }
    out
}

/// Whether an authorized_keys file has this key. Lines may start with options,
/// whose quoted values may hold spaces.
fn lists(file: &str, key: &str) -> bool {
    file.lines().any(|line| {
        if line.trim_start().starts_with('#') {
            return false;
        }
        let mut quoted = false;
        let mut words = line.split(|c| {
            quoted ^= c == '"';
            !quoted && c == ' '
        });
        while let Some(word) = words.next() {
            if word == "ssh-ed25519" {
                return words.next() == Some(key);
            }
        }
        false
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: &str = "AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    #[test]
    fn the_blob_is_openssh_s() {
        assert_eq!(base64(&ssh_blob(&[0; 32])), KEY);
    }

    #[test]
    fn keys_are_found_with_or_without_options() {
        assert!(lists(&format!("ssh-ed25519 {KEY} tim@mac\n"), KEY));
        assert!(lists(&format!("ssh-rsa AAAA a\nrestrict,command=\"/bin/false\" ssh-ed25519 {KEY} gmstream\n"), KEY));
        assert!(lists(&format!("command=\"echo a b\" ssh-ed25519 {KEY}"), KEY));
    }

    #[test]
    fn nothing_else_counts() {
        assert!(!lists("", KEY));
        assert!(!lists(&format!("# ssh-ed25519 {KEY}"), KEY));
        assert!(!lists("ssh-ed25519 AAAAother tim@mac", KEY));
        assert!(!lists(&format!("command=\"ssh-ed25519 {KEY}\" ssh-rsa AAAA"), KEY));
    }

    #[test]
    fn a_number_of_one_kind_is_a_different_nonce_from_another_s() {
        assert_ne!(nonce(Kind::Control, 5).as_ref(), nonce(Kind::Mouse, 5).as_ref());
        assert_ne!(nonce(Kind::Mouse, 5).as_ref(), nonce(Kind::Ack, 5).as_ref());
    }

    #[test]
    fn each_side_opens_what_the_other_seals_and_nothing_else() {
        let dir = std::env::temp_dir().join(format!("gm-auth-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let host = HostKey::load(&dir).unwrap();
        assert_eq!(host.0.public_key().as_ref(), HostKey::load(&dir).unwrap().0.public_key().as_ref(), "kept");
        std::fs::remove_dir_all(&dir).unwrap();
        let a = EphemeralPrivateKey::generate(&X25519, &SystemRandom::new()).unwrap();
        let public = a.compute_public_key().unwrap();
        let (_, keys) = respond(&host, public.as_ref()).unwrap();
        let sealed = keys.seal(Kind::Control, b"h", 7, b"hello");
        assert_eq!(sealed.len(), 5 + TAG_SIZE);
        assert!(keys.open(Kind::Control, b"h", 7, &sealed).is_none(), "the directions have different keys");
    }
}
