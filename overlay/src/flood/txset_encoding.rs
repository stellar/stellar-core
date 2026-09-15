//! Cached TX set encoding for the compressed response protocol.
//!
//! Inside the usual length-prefixed frame: a big-endian u32 XDR length,
//! followed by one zstd frame. The high bit marks uncompressed XDR when
//! compression would not save space. Both encodings retain the legacy limit.
use std::{io, sync::Arc};
use tokio::sync::OnceCell;

pub(crate) const MAX_TXSET_XDR_SIZE: usize = 16 * 1024 * 1024 - 4;
const RAW: u32 = 1 << 31;

/// One immutable allocation per cached set, shared by all peer sends. The
/// compressed representation is computed on demand once for the whole fanout.
#[derive(Debug)]
pub struct TxSetData {
    xdr: Vec<u8>,
    compressed: OnceCell<Option<Vec<u8>>>,
}

impl From<Vec<u8>> for TxSetData {
    fn from(xdr: Vec<u8>) -> Self {
        Self {
            xdr,
            compressed: OnceCell::new(),
        }
    }
}

impl TxSetData {
    pub fn as_slice(&self) -> &[u8] {
        &self.xdr
    }

    pub fn len(&self) -> usize {
        self.xdr.len()
    }

    pub fn is_empty(&self) -> bool {
        self.xdr.is_empty()
    }

    pub(crate) async fn encoded(self: &Arc<Self>) -> io::Result<([u8; 4], &[u8])> {
        let compressed = self
            .compressed
            .get_or_try_init(|| async {
                let data = Arc::clone(self);
                // Compression must not occupy a Tokio network worker. Admission
                // already bounds the number and raw bytes of outstanding sends.
                tokio::task::spawn_blocking(move || compress(&data.xdr))
                    .await
                    .map_err(io::Error::other)?
            })
            .await?;
        let length = self.len() as u32;
        Ok(match compressed {
            Some(bytes) => (length.to_be_bytes(), bytes),
            None => ((length | RAW).to_be_bytes(), &self.xdr),
        })
    }
}

fn compress(xdr: &[u8]) -> io::Result<Option<Vec<u8>>> {
    if xdr.len() > MAX_TXSET_XDR_SIZE {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "TX set exceeds wire limit",
        ));
    }
    let mut bytes = zstd::bulk::compress(xdr, 1)?;
    if bytes.len() >= xdr.len() {
        return Ok(None);
    }
    // bulk::compress reserves the worst-case size, approximately the raw
    // length. Do not retain that spare allocation throughout the cache lifetime.
    bytes.shrink_to_fit();
    Ok(Some(bytes))
}

pub(crate) fn decode(mut frame: Vec<u8>) -> io::Result<Vec<u8>> {
    let invalid = || io::Error::new(io::ErrorKind::InvalidData, "invalid TX set encoding");
    if frame.len() < 4 || frame.len() > MAX_TXSET_XDR_SIZE + 4 {
        return Err(invalid());
    }
    let header = u32::from_be_bytes(frame[..4].try_into().unwrap());
    let length = (header & !RAW) as usize;
    // Check before any decompressor or output allocation. Never trust the
    // content size embedded in the compressed frame to allocate memory.
    if length > MAX_TXSET_XDR_SIZE {
        return Err(invalid());
    }
    if header & RAW != 0 {
        if frame.len() - 4 != length {
            return Err(invalid());
        }
        frame.drain(..4);
        return Ok(frame);
    }
    let compressed = &frame[4..];
    if zstd::zstd_safe::find_frame_compressed_size(compressed).map_err(|_| invalid())?
        != compressed.len()
    {
        return Err(invalid());
    }
    let mut decoder = zstd::bulk::Decompressor::new()?;
    decoder.window_log_max(24)?;
    let output = decoder.decompress(compressed, length)?;
    if output.len() != length {
        return Err(invalid());
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rand::{RngCore, SeedableRng};

    async fn encode(xdr: Vec<u8>) -> Vec<u8> {
        let data = Arc::new(TxSetData::from(xdr));
        let (header, bytes) = data.encoded().await.unwrap();
        [header.as_slice(), bytes].concat()
    }

    #[tokio::test]
    async fn roundtrip_and_size_bound_for_both_encodings() {
        let mut random = vec![0; 64 * 1024];
        rand::rngs::StdRng::seed_from_u64(17).fill_bytes(&mut random);
        for xdr in [vec![], vec![17], random, vec![0; MAX_TXSET_XDR_SIZE]] {
            let frame = encode(xdr.clone()).await;
            assert!(frame.len() <= xdr.len() + 4);
            assert_eq!(decode(frame).unwrap(), xdr);
        }
        let raw = encode(vec![17]).await;
        assert_ne!(raw[0] & 0x80, 0);
        let compressed = encode(vec![17; 1024]).await;
        assert_eq!(compressed[0] & 0x80, 0);
        assert!(compressed.len() < 100);
    }

    #[tokio::test]
    async fn concurrent_fanout_shares_one_encoded_allocation() {
        let data = Arc::new(TxSetData::from(vec![13; 4 * 1024 * 1024]));
        let results = futures::future::join_all((0..29).map(|_| data.encoded())).await;
        let (header, bytes) = results[0].as_ref().unwrap();
        assert_eq!(header[0] & 0x80, 0);
        for result in &results {
            let (other_header, other_bytes) = result.as_ref().unwrap();
            assert_eq!(other_header, header);
            assert!(std::ptr::eq(bytes.as_ptr(), other_bytes.as_ptr()));
        }
        let (_, again) = data.encoded().await.unwrap();
        assert!(std::ptr::eq(bytes.as_ptr(), again.as_ptr()));
    }

    #[tokio::test]
    async fn rejects_truncation_corruption_and_length_mismatch() {
        let frame = encode(vec![13; 4096]).await;
        for end in 0..frame.len() {
            assert!(decode(frame[..end].to_vec()).is_err());
        }
        let mut corrupt = frame.clone();
        corrupt[4] ^= 0xff; // Invalid zstd magic.
        assert!(decode(corrupt).is_err());
        for length in [0, 4095, 4097, MAX_TXSET_XDR_SIZE + 1] {
            let mut bad = frame.clone();
            bad[..4].copy_from_slice(&(length as u32).to_be_bytes());
            assert!(decode(bad).is_err());
        }
        // Exactly one zstd frame must consume the entire payload.
        let mut trailing = frame.clone();
        trailing.push(0);
        assert!(decode(trailing).is_err());
        let mut concatenated = frame.clone();
        concatenated.extend_from_slice(&frame[4..]);
        assert!(decode(concatenated).is_err());
        let mut raw = encode(vec![19]).await;
        raw.push(0);
        assert!(decode(raw).is_err());
    }

    #[tokio::test]
    async fn compression_does_not_allow_oversized_raw_sets() {
        let oversized = vec![0; MAX_TXSET_XDR_SIZE + 1];
        let data = Arc::new(TxSetData::from(oversized));
        assert!(data.encoded().await.is_err());
        // Even if its compressed bytes fit, an over-limit raw size is rejected.
        let frame = [
            (MAX_TXSET_XDR_SIZE as u32 + 1).to_be_bytes().as_slice(),
            zstd::bulk::compress(data.as_slice(), 1).unwrap().as_slice(),
        ]
        .concat();
        assert!(decode(frame).is_err());
        assert!(decode(vec![0; MAX_TXSET_XDR_SIZE + 5]).is_err());
    }
}
