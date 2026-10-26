//! The two ways a client reaches a session: WebTransport (browsers, UDP port
//! P) and plain UDP (native clients, UDP port P + 1, see udp.rs). Both carry
//! the same control messages, video and audio (PROTOCOL.md); the session code
//! only sees these wrappers.

use std::net::SocketAddr;
use std::sync::Arc;

use anyhow::{Result, bail};
use wtransport::VarInt;

use crate::udp;

pub enum Conn {
    Wt(wtransport::Connection),
    Udp(Arc<udp::Peer>),
}

pub enum SendStream {
    Wt(wtransport::SendStream),
    /// The host → client control stream over UDP.
    Udp(Arc<udp::Peer>),
}

pub enum RecvStream {
    Wt(wtransport::RecvStream),
    Udp(udp::Control),
}

impl Conn {
    pub fn remote_address(&self) -> SocketAddr {
        match self {
            Conn::Wt(c) => c.remote_address(),
            Conn::Udp(p) => p.addr,
        }
    }

    /// UDP clients have no streams: their video always comes as datagrams.
    pub fn is_udp(&self) -> bool {
        matches!(self, Conn::Udp(_))
    }

    /// Opens a unidirectional stream; waits while the client grants no
    /// stream credit.
    pub async fn open_uni(&self) -> Result<SendStream> {
        match self {
            Conn::Wt(c) => Ok(SendStream::Wt(c.open_uni().await?.await?)),
            Conn::Udp(_) => bail!("plain UDP has no streams"),
        }
    }

    /// Sends now; a full buffer drops the datagram.
    pub fn send_datagram(&self, d: Vec<u8>) -> Result<()> {
        match self {
            Conn::Wt(c) => c.send_datagram(d)?,
            Conn::Udp(p) => p.try_send(&d)?,
        }
        Ok(())
    }

    /// Sends, waiting for buffer room where the transport needs it (video).
    pub async fn send_datagram_wait(&self, d: Vec<u8>) -> Result<()> {
        match self {
            Conn::Wt(c) => c.send_datagram(d)?,
            Conn::Udp(p) => p.send(&d).await?,
        }
        Ok(())
    }

    /// Several datagrams in order, waiting for buffer room; batched into few
    /// system calls on plain UDP.
    pub async fn send_datagrams_wait(&self, ds: Vec<Vec<u8>>) -> Result<()> {
        match self {
            Conn::Wt(c) => {
                for d in ds {
                    c.send_datagram(d)?;
                }
            }
            Conn::Udp(p) => p.send_batch(&ds).await?,
        }
        Ok(())
    }

    pub fn max_datagram_size(&self) -> Option<usize> {
        match self {
            Conn::Wt(c) => c.max_datagram_size(),
            Conn::Udp(_) => Some(udp::MAX_DATAGRAM),
        }
    }

    pub fn close(&self, code: u32, reason: &[u8]) {
        match self {
            Conn::Wt(c) => c.close(VarInt::from_u32(code), reason),
            Conn::Udp(p) => p.close(),
        }
    }
}

impl SendStream {
    pub async fn write_all(&mut self, buf: &[u8]) -> Result<()> {
        match self {
            SendStream::Wt(s) => s.write_all(buf).await?,
            SendStream::Udp(p) => p.send_control(buf).await?,
        }
        Ok(())
    }

    pub async fn finish(&mut self) -> Result<()> {
        if let SendStream::Wt(s) = self {
            s.finish().await?;
        }
        Ok(())
    }

    pub fn reset(&mut self, code: u32) {
        if let SendStream::Wt(s) = self {
            let _ = s.reset(VarInt::from_u32(code));
        }
    }

    pub fn set_priority(&self, priority: i32) {
        if let SendStream::Wt(s) = self {
            s.set_priority(priority);
        }
    }
}

impl RecvStream {
    pub async fn read_exact(&mut self, buf: &mut [u8]) -> Result<()> {
        match self {
            RecvStream::Wt(s) => s.read_exact(buf).await?,
            RecvStream::Udp(c) => c.read_exact(buf).await?,
        }
        Ok(())
    }
}
