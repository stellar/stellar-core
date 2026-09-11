//! INV/GETDATA message types for bandwidth-efficient TX flooding.
//!
//! Wire format: length-prefixed `StellarMessage` XDR.

use std::io;
use std::sync::Arc;
use stellar_xdr::curr::{
    FloodAdvert, FloodDemand, Hash, Limits, ReadXdr, StellarMessage, TxAdvertVector,
    TxDemandVector, WriteXdr, TX_ADVERT_VECTOR_MAX_SIZE, TX_DEMAND_VECTOR_MAX_SIZE,
};

use crate::wire::ValidatedTx;

/// A single INV entry: hash + fee for prioritization
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InvEntry {
    /// SHA256 hash of the transaction
    pub hash: [u8; 32],
    /// Fee per operation (for smart GETDATA prioritization)
    pub fee_per_op: i64,
}

/// Maximum entries in a single INV_BATCH message
pub const INV_BATCH_MAX_SIZE: usize = 1000;

/// Batch of transaction inventory announcements
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InvBatch {
    pub entries: Vec<InvEntry>,
}

impl InvBatch {
    /// Create a new empty batch
    pub fn new() -> Self {
        InvBatch {
            entries: Vec::new(),
        }
    }

    /// Add an entry to the batch
    pub fn push(&mut self, entry: InvEntry) {
        self.entries.push(entry);
    }

    /// Encode as one or more `StellarMessage::FloodAdvert` XDR messages,
    /// splitting the entries so no message exceeds the `TxAdvertVector` XDR
    /// bound (`TX_ADVERT_VECTOR_MAX_SIZE`).
    ///
    /// Each chunk is returned with the entries it carries, so callers can
    /// track per-chunk delivery.
    pub fn encode_chunked(&self) -> io::Result<Vec<(Vec<u8>, Vec<InvEntry>)>> {
        self.entries
            .chunks(TX_ADVERT_VECTOR_MAX_SIZE as usize)
            .map(|chunk| {
                let hashes = chunk.iter().map(|e| Hash(e.hash)).collect::<Vec<_>>();
                let tx_hashes = TxAdvertVector::try_from(hashes).map_err(to_invalid_data)?;
                let encoded = StellarMessage::FloodAdvert(FloodAdvert { tx_hashes })
                    .to_xdr(Limits::none())
                    .map_err(to_invalid_data)?;
                Ok((encoded, chunk.to_vec()))
            })
            .collect()
    }

    /// Encode as a single `StellarMessage::FloodAdvert` XDR.
    ///
    /// Returns an error if the batch exceeds `TX_ADVERT_VECTOR_MAX_SIZE`.
    /// Prefer `encode_chunked()` for batches that may exceed the limit.
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        let hashes = self
            .entries
            .iter()
            .map(|e| Hash(e.hash))
            .collect::<Vec<_>>();
        let tx_hashes = TxAdvertVector::try_from(hashes).map_err(to_invalid_data)?;
        StellarMessage::FloodAdvert(FloodAdvert { tx_hashes })
            .to_xdr(Limits::none())
            .map_err(to_invalid_data)
    }
}

impl Default for InvBatch {
    fn default() -> Self {
        Self::new()
    }
}

/// Request for specific transactions by hash
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GetData {
    pub hashes: Vec<[u8; 32]>,
}

impl GetData {
    pub fn new() -> Self {
        GetData { hashes: Vec::new() }
    }

    pub fn push(&mut self, hash: [u8; 32]) {
        self.hashes.push(hash);
    }

    /// Encode as one or more `StellarMessage::FloodDemand` XDR messages,
    /// splitting the hashes so no message exceeds the `TxDemandVector` XDR
    /// bound (`TX_DEMAND_VECTOR_MAX_SIZE`).
    ///
    /// Each chunk is returned with the hashes it carries, so callers can
    /// track per-hash delivery (e.g. stamp request timestamps only after the
    /// chunk actually went out on the wire).
    pub fn encode_chunked(&self) -> io::Result<Vec<(Vec<u8>, Vec<[u8; 32]>)>> {
        self.hashes
            .chunks(TX_DEMAND_VECTOR_MAX_SIZE as usize)
            .map(|chunk| {
                let hashes = chunk.iter().map(|hash| Hash(*hash)).collect::<Vec<_>>();
                let tx_hashes = TxDemandVector::try_from(hashes).map_err(to_invalid_data)?;
                let encoded = StellarMessage::FloodDemand(FloodDemand { tx_hashes })
                    .to_xdr(Limits::none())
                    .map_err(to_invalid_data)?;
                Ok((encoded, chunk.to_vec()))
            })
            .collect()
    }
}

impl Default for GetData {
    fn default() -> Self {
        Self::new()
    }
}

/// Parsed TX stream message.
///
/// The `Tx` arm carries an already-validated `Arc<ValidatedTx>`: `decode`
/// performs the single strict `StellarMessage` decode this module is allowed to
/// do, then mints the tx from the decoded envelope without re-decoding.
///
/// `FloodAdvert` and `FloodDemand` are **not** valid on the TX stream; they
/// have been promoted to the SCP (control) stream so they are not head-of-line
/// blocked behind bulk transaction payloads.
#[derive(Debug, Clone)]
pub enum TxStreamMessage {
    /// A validated transaction
    Tx(Arc<ValidatedTx>),
}

impl TxStreamMessage {
    /// Decode a `StellarMessage` off the TX stream.
    ///
    /// This is a trust boundary: `data` came from a peer. The single decode here
    /// both validates the message and, for transactions, produces the
    /// `ValidatedTx` (minted from the decoded envelope and its original bytes,
    /// `data[4..]`, after the 4-byte union discriminant).
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        match StellarMessage::from_xdr(data, Limits::none()).map_err(to_invalid_data)? {
            StellarMessage::Transaction(envelope) => {
                let tx =
                    ValidatedTx::from_network(&envelope, &data[4..]).map_err(to_invalid_data)?;
                Ok(TxStreamMessage::Tx(tx))
            }
            other => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "unexpected TX stream StellarMessage {} (adverts/demands belong on SCP stream)",
                    other.name()
                ),
            )),
        }
    }
}

/// Parsed SCP control-stream message (control traffic promoted above bulk
/// transaction flooding).
///
/// Consensus-related SCP envelopes are handled directly by the SCP stream
/// reader in `libp2p_overlay`; this decoder covers the additional control
/// messages that now share the high-priority SCP stream: TX adverts
/// (`FloodAdvert`) and TX demands (`FloodDemand`).
#[derive(Debug, Clone)]
pub enum ScpControlMessage {
    /// Batch of INV announcements (advertised TX hashes)
    InvBatch(InvBatch),
    /// Request for transactions by hash (demand)
    GetData(GetData),
}

impl ScpControlMessage {
    /// Decode a `StellarMessage` off the SCP control stream.
    ///
    /// Only `FloodAdvert` and `FloodDemand` are accepted; all other message
    /// types belong to a different stream and are rejected.
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        match StellarMessage::from_xdr(data, Limits::none()).map_err(to_invalid_data)? {
            StellarMessage::FloodAdvert(advert) => {
                let entries = advert
                    .tx_hashes
                    .iter()
                    .map(|hash| InvEntry {
                        hash: hash.0,
                        fee_per_op: 0,
                    })
                    .collect();
                Ok(ScpControlMessage::InvBatch(InvBatch { entries }))
            }
            StellarMessage::FloodDemand(demand) => {
                let hashes = demand.tx_hashes.iter().map(|hash| hash.0).collect();
                Ok(ScpControlMessage::GetData(GetData { hashes }))
            }
            other => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "unexpected SCP control stream StellarMessage {}",
                    other.name()
                ),
            )),
        }
    }
}

fn to_invalid_data<E: std::fmt::Display>(err: E) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, err.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xdr::tests::valid_transaction_xdr;

    #[test]
    fn test_tx_stream_message_tx() {
        let tx_data = valid_transaction_xdr(1000, 1, 1);
        let encoded = crate::xdr::frame_transaction(&tx_data);

        match TxStreamMessage::decode(&encoded).unwrap() {
            TxStreamMessage::Tx(tx) => assert_eq!(tx.bytes(), &tx_data[..]),
        }
    }

    #[test]
    fn test_tx_stream_rejects_flood_advert() {
        let mut batch = InvBatch::new();
        batch.push(InvEntry {
            hash: [0x42; 32],
            fee_per_op: 500,
        });
        let encoded = batch.encode().unwrap();
        let result = TxStreamMessage::decode(&encoded);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("SCP stream"));
    }

    #[test]
    fn test_tx_stream_rejects_flood_demand() {
        let mut gd = GetData::new();
        gd.push([0xFF; 32]);
        let chunks = gd.encode_chunked().unwrap();
        let (encoded, _) = &chunks[0];
        let result = TxStreamMessage::decode(encoded);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("SCP stream"));
    }

    #[test]
    fn test_scp_control_inv_batch() {
        let mut batch = InvBatch::new();
        batch.push(InvEntry {
            hash: [0x42; 32],
            fee_per_op: 500,
        });
        let encoded = batch.encode().unwrap();
        let decoded = ScpControlMessage::decode(&encoded).unwrap();
        if let ScpControlMessage::InvBatch(decoded_batch) = decoded {
            assert_eq!(decoded_batch.entries.len(), 1);
            assert_eq!(decoded_batch.entries[0].hash, batch.entries[0].hash);
        } else {
            panic!("Expected InvBatch");
        }
    }

    #[test]
    fn test_scp_control_getdata() {
        let mut gd = GetData::new();
        gd.push([0xFF; 32]);
        let chunks = gd.encode_chunked().unwrap();
        assert_eq!(chunks.len(), 1);
        let (encoded, chunk_hashes) = &chunks[0];
        assert_eq!(chunk_hashes, &gd.hashes);
        let decoded = ScpControlMessage::decode(encoded).unwrap();
        if let ScpControlMessage::GetData(decoded_gd) = decoded {
            assert_eq!(gd, decoded_gd);
        } else {
            panic!("Expected GetData");
        }
    }

    #[test]
    fn test_getdata_encode_chunked_splits_at_xdr_bound() {
        let max = TX_DEMAND_VECTOR_MAX_SIZE as usize;
        let mut gd = GetData::new();
        for i in 0..(max * 2 + 5) {
            let mut hash = [0u8; 32];
            hash[..8].copy_from_slice(&(i as u64).to_be_bytes());
            gd.push(hash);
        }

        // Chunked encode must split it into decodable messages that
        // round-trip every hash in order, and report the hashes carried by
        // each chunk so callers can track per-chunk delivery.
        let chunks = gd.encode_chunked().unwrap();
        assert_eq!(chunks.len(), 3);
        let mut decoded_hashes = Vec::new();
        let mut reported_hashes = Vec::new();
        for (encoded, chunk_hashes) in &chunks {
            match ScpControlMessage::decode(encoded).unwrap() {
                ScpControlMessage::GetData(decoded) => {
                    assert!(decoded.hashes.len() <= max);
                    // The reported hashes must match the encoded content.
                    assert_eq!(&decoded.hashes, chunk_hashes);
                    decoded_hashes.extend(decoded.hashes);
                }
                _ => panic!("Expected GetData"),
            }
            reported_hashes.extend(chunk_hashes.iter().copied());
        }
        assert_eq!(decoded_hashes, gd.hashes);
        assert_eq!(reported_hashes, gd.hashes);
    }

    #[test]
    fn test_decode_empty_message_fails() {
        let result = TxStreamMessage::decode(&[]);
        assert!(result.is_err());
    }

    #[test]
    fn test_scp_control_decode_empty_fails() {
        let result = ScpControlMessage::decode(&[]);
        assert!(result.is_err());
    }

    #[test]
    fn test_decode_unknown_type_fails() {
        let result = TxStreamMessage::decode(&[0xFF, 0x01, 0x02]);
        assert!(result.is_err());
    }

    #[test]
    fn test_inv_batch_encode_chunked_splits_at_xdr_bound() {
        let max = TX_ADVERT_VECTOR_MAX_SIZE as usize;
        let mut batch = InvBatch::new();
        for i in 0..(max * 2 + 5) {
            let mut hash = [0u8; 32];
            hash[..8].copy_from_slice(&(i as u64).to_be_bytes());
            batch.push(InvEntry {
                hash,
                fee_per_op: (i as i64) * 10,
            });
        }

        let chunks = batch.encode_chunked().unwrap();
        assert_eq!(chunks.len(), 3);

        let mut decoded_entries = Vec::new();
        let mut reported_entries = Vec::new();
        for (encoded, chunk_entries) in &chunks {
            match ScpControlMessage::decode(encoded).unwrap() {
                ScpControlMessage::InvBatch(decoded_batch) => {
                    assert!(decoded_batch.entries.len() <= max);
                    assert_eq!(&decoded_batch.entries, chunk_entries);
                    decoded_entries.extend(decoded_batch.entries);
                }
                _ => panic!("Expected InvBatch"),
            }
            reported_entries.extend(chunk_entries.iter().cloned());
        }
        assert_eq!(decoded_entries, batch.entries);
        assert_eq!(reported_entries, batch.entries);
    }

    #[test]
    fn test_inv_batch_encode_chunked_small_batch() {
        let mut batch = InvBatch::new();
        for i in 0..5 {
            let mut hash = [0u8; 32];
            hash[0] = i;
            batch.push(InvEntry {
                hash,
                fee_per_op: 100,
            });
        }

        let chunks = batch.encode_chunked().unwrap();
        assert_eq!(chunks.len(), 1);
        let (encoded, chunk_entries) = &chunks[0];
        assert_eq!(chunk_entries.len(), 5);

        // Verify round-trip
        let decoded = ScpControlMessage::decode(encoded).unwrap();
        if let ScpControlMessage::InvBatch(decoded_batch) = decoded {
            assert_eq!(decoded_batch.entries.len(), 5);
            for (i, entry) in decoded_batch.entries.iter().enumerate() {
                assert_eq!(entry.hash[0], i as u8);
                assert_eq!(entry.fee_per_op, 100);
            }
        } else {
            panic!("Expected InvBatch");
        }
    }
}
