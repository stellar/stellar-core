//! Immutable TX set bytes and their mandatory zstd response encoding.
//!
//! Inside the length-prefixed response: a big-endian u32 XDR length followed
//! by exactly one zstd frame. Only locally built sets are compressed; received
//! sets retain the original response bytes for forwarding.
use std::io;

pub(crate) const MAX_TXSET_XDR_SIZE: usize = 16 * 1024 * 1024 - 4;

/// Allow zstd's worst-case expansion while retaining the original XDR limit.
pub(crate) fn max_encoded_size() -> usize {
    4 + zstd::zstd_safe::compress_bound(MAX_TXSET_XDR_SIZE)
}

#[derive(Debug)]
pub struct TxSetData {
    xdr: Vec<u8>,
    encoded: Vec<u8>,
}

impl TxSetData {
    /// Compress a locally constructed set eagerly, on a blocking worker.
    pub fn from_local(xdr: Vec<u8>) -> io::Result<Self> {
        if xdr.len() > MAX_TXSET_XDR_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "TX set exceeds XDR limit",
            ));
        }
        let compressed = zstd::bulk::compress(&xdr, 1)?;
        let mut encoded = Vec::with_capacity(4 + compressed.len());
        encoded.extend_from_slice(&(xdr.len() as u32).to_be_bytes());
        encoded.extend_from_slice(&compressed);
        Ok(Self { xdr, encoded })
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.xdr
    }

    pub fn len(&self) -> usize {
        self.xdr.len()
    }

    pub fn is_empty(&self) -> bool {
        self.xdr.is_empty()
    }

    /// Borrow the exact cached response; sending never invokes a compressor.
    pub fn encoded(&self) -> &[u8] {
        &self.encoded
    }
}

/// Decode on a blocking worker and retain the received allocation unchanged.
/// The caller must also strict-parse the recovered GeneralizedTransactionSet.
pub(crate) fn decode(frame: Vec<u8>) -> io::Result<TxSetData> {
    let invalid = || io::Error::new(io::ErrorKind::InvalidData, "invalid TX set encoding");
    if frame.len() < 4 || frame.len() > max_encoded_size() {
        return Err(invalid());
    }
    let length = u32::from_be_bytes(frame[..4].try_into().unwrap()) as usize;
    // Check before creating a decoder or allocating its output.
    if length > MAX_TXSET_XDR_SIZE {
        return Err(invalid());
    }
    let compressed = &frame[4..];
    if zstd::zstd_safe::find_frame_compressed_size(compressed).map_err(|_| invalid())?
        != compressed.len()
    {
        return Err(invalid());
    }
    let mut decoder = zstd::bulk::Decompressor::new()?;
    decoder.window_log_max(24)?;
    let xdr = decoder.decompress(compressed, length)?;
    if xdr.len() != length {
        return Err(invalid());
    }
    Ok(TxSetData {
        xdr,
        encoded: frame,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use rand::{RngCore, SeedableRng};
    use std::sync::Arc;

    #[test]
    fn always_compresses_including_empty_and_incompressible_sets() {
        let mut random = vec![0; MAX_TXSET_XDR_SIZE];
        rand::rngs::StdRng::seed_from_u64(17).fill_bytes(&mut random);
        for xdr in [vec![], vec![17], random, vec![0; MAX_TXSET_XDR_SIZE]] {
            let data = TxSetData::from_local(xdr.clone()).unwrap();
            let frame = data.encoded().to_vec();
            assert!(frame.len() <= max_encoded_size());
            assert_eq!(
                u32::from_be_bytes(frame[..4].try_into().unwrap()) as usize,
                xdr.len()
            );
            assert_eq!(zstd::bulk::decompress(&frame[4..], xdr.len()).unwrap(), xdr);
            let received = decode(frame.clone()).unwrap();
            assert_eq!(received.as_slice(), xdr);
            assert_eq!(received.encoded(), frame);
        }
    }

    #[test]
    fn fanout_shares_the_prepared_encoding() {
        let data = Arc::new(TxSetData::from_local(vec![13; 4 * 1024 * 1024]).unwrap());
        let peers: Vec<_> = (0..29).map(|_| Arc::clone(&data)).collect();
        for peer in peers {
            assert!(std::ptr::eq(
                data.encoded().as_ptr(),
                peer.encoded().as_ptr()
            ));
        }
    }

    #[test]
    fn received_encoding_is_retained_without_recompression() {
        let xdr = vec![13; 4 * 1024 * 1024];
        // Deliberately use another compression level. Relays must preserve these
        // bytes even when their own encoder would produce a different frame.
        let frame = [
            (xdr.len() as u32).to_be_bytes().as_slice(),
            zstd::bulk::compress(&xdr, 7).unwrap().as_slice(),
        ]
        .concat();
        let allocation = frame.as_ptr();
        let expected = frame.clone();
        let received = decode(frame).unwrap();
        assert_eq!(received.as_slice(), xdr);
        assert_eq!(received.encoded(), expected);
        assert_eq!(received.encoded().as_ptr(), allocation);
    }

    #[test]
    fn rejects_truncation_corruption_length_mismatch_and_legacy_raw() {
        let frame = TxSetData::from_local(vec![13; 4096]).unwrap().encoded;
        for end in 0..frame.len() {
            assert!(decode(frame[..end].to_vec()).is_err());
        }
        let mut corrupt = frame.clone();
        corrupt[4] ^= 0xff;
        assert!(decode(corrupt).is_err());
        for length in [0, 4095, 4097, MAX_TXSET_XDR_SIZE + 1, 4096 | (1 << 31)] {
            let mut bad = frame.clone();
            bad[..4].copy_from_slice(&(length as u32).to_be_bytes());
            assert!(decode(bad).is_err());
        }
        let mut trailing = frame.clone();
        trailing.push(0);
        assert!(decode(trailing).is_err());
        let mut concatenated = frame.clone();
        concatenated.extend_from_slice(&frame[4..]);
        assert!(decode(concatenated).is_err());
        let raw = [19u32.to_be_bytes().as_slice(), &[19; 19]].concat();
        assert!(decode(raw).is_err());
    }

    #[test]
    fn compression_does_not_allow_oversized_raw_sets() {
        let oversized = vec![0; MAX_TXSET_XDR_SIZE + 1];
        assert!(TxSetData::from_local(oversized.clone()).is_err());
        let frame = [
            (oversized.len() as u32).to_be_bytes().as_slice(),
            zstd::bulk::compress(&oversized, 1).unwrap().as_slice(),
        ]
        .concat();
        assert!(decode(frame).is_err());
        assert!(decode(vec![0; max_encoded_size() + 1]).is_err());
    }
}
