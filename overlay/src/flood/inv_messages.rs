//! INV/GETDATA message types for bandwidth-efficient TX flooding.
//!
//! Wire format: length-prefixed `StellarMessage` XDR.

use std::io;
use std::sync::Arc;
use stellar_xdr::curr::{
    FloodAdvert, FloodDemand, Hash, Limits, ReadXdr, StellarMessage, TxAdvertVector,
    TxDemandVector, WriteXdr,
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

    /// Encode as a `StellarMessage::FloodAdvert` XDR.
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

    /// Encode as a `StellarMessage::FloodDemand` XDR.
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        let hashes = self
            .hashes
            .iter()
            .map(|hash| Hash(*hash))
            .collect::<Vec<_>>();
        let tx_hashes = TxDemandVector::try_from(hashes).map_err(to_invalid_data)?;
        StellarMessage::FloodDemand(FloodDemand { tx_hashes })
            .to_xdr(Limits::none())
            .map_err(to_invalid_data)
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
#[derive(Debug, Clone)]
pub enum TxStreamMessage {
    /// A validated transaction
    Tx(Arc<ValidatedTx>),
    /// Batch of INV announcements
    InvBatch(InvBatch),
    /// Request for transactions
    GetData(GetData),
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
            StellarMessage::FloodAdvert(advert) => {
                let entries = advert
                    .tx_hashes
                    .iter()
                    .map(|hash| InvEntry {
                        hash: hash.0,
                        fee_per_op: 0,
                    })
                    .collect();
                Ok(TxStreamMessage::InvBatch(InvBatch { entries }))
            }
            StellarMessage::FloodDemand(demand) => {
                let hashes = demand.tx_hashes.iter().map(|hash| hash.0).collect();
                Ok(TxStreamMessage::GetData(GetData { hashes }))
            }
            other => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("unexpected TX stream StellarMessage {}", other.name()),
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
            _ => panic!("Expected Tx"),
        }
    }

    #[test]
    fn test_tx_stream_message_inv_batch() {
        let mut batch = InvBatch::new();
        batch.push(InvEntry {
            hash: [0x42; 32],
            fee_per_op: 500,
        });
        let encoded = batch.encode().unwrap();
        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        if let TxStreamMessage::InvBatch(decoded_batch) = decoded {
            assert_eq!(decoded_batch.entries.len(), 1);
            assert_eq!(decoded_batch.entries[0].hash, batch.entries[0].hash);
        } else {
            panic!("Expected InvBatch");
        }
    }

    #[test]
    fn test_tx_stream_message_getdata() {
        let mut gd = GetData::new();
        gd.push([0xFF; 32]);
        let encoded = gd.encode().unwrap();
        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        if let TxStreamMessage::GetData(decoded_gd) = decoded {
            assert_eq!(gd, decoded_gd);
        } else {
            panic!("Expected GetData");
        }
    }

    #[test]
    fn test_decode_empty_message_fails() {
        let result = TxStreamMessage::decode(&[]);
        assert!(result.is_err());
    }

    #[test]
    fn test_decode_unknown_type_fails() {
        let result = TxStreamMessage::decode(&[0xFF, 0x01, 0x02]);
        assert!(result.is_err());
    }
}
