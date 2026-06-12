//! TX stream message types for transaction flooding.
//!
//! Wire format: length-prefixed payloads. INV/GETDATA messages are encoded as
//! `StellarMessage` XDR. TX messages are encoded as a big-endian TTL followed by
//! `StellarMessage::Transaction` XDR; legacy raw transaction messages decode
//! with TTL 0.

use std::io;
use stellar_xdr::curr::{
    FloodAdvert, FloodDemand, Hash, Limits, ReadXdr, StellarMessage, TxAdvertVector,
    TxDemandVector, WriteXdr,
};

const TX_TTL_BYTES: usize = 4;

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
}

impl Default for GetData {
    fn default() -> Self {
        Self::new()
    }
}

/// Full transaction data carried on the TX stream.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TxMessage {
    pub tx: Vec<u8>,
    pub ttl: u32,
}

/// Parsed TX stream message
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TxStreamMessage {
    /// Full transaction data
    Tx(TxMessage),
    /// Batch of INV announcements
    InvBatch(InvBatch),
    /// Request for transactions
    GetData(GetData),
}

impl TxStreamMessage {
    /// Encode message as StellarMessage XDR.
    pub fn encode(&self) -> io::Result<Vec<u8>> {
        match self {
            TxStreamMessage::Tx(tx_message) => {
                let message = crate::xdr::encode_transaction_message_from_xdr(&tx_message.tx)
                    .map_err(to_invalid_data)?;
                let mut encoded = Vec::with_capacity(TX_TTL_BYTES + message.len());
                encoded.extend_from_slice(&tx_message.ttl.to_be_bytes());
                encoded.extend_from_slice(&message);
                Ok(encoded)
            }
            TxStreamMessage::InvBatch(batch) => {
                let hashes = batch
                    .entries
                    .iter()
                    .map(|entry| Hash(entry.hash))
                    .collect::<Vec<_>>();
                let tx_hashes = TxAdvertVector::try_from(hashes).map_err(to_invalid_data)?;
                let message = StellarMessage::FloodAdvert(FloodAdvert { tx_hashes });
                message.to_xdr(Limits::none()).map_err(to_invalid_data)
            }
            TxStreamMessage::GetData(gd) => {
                let hashes = gd.hashes.iter().map(|hash| Hash(*hash)).collect::<Vec<_>>();
                let tx_hashes = TxDemandVector::try_from(hashes).map_err(to_invalid_data)?;
                let message = StellarMessage::FloodDemand(FloodDemand { tx_hashes });
                message.to_xdr(Limits::none()).map_err(to_invalid_data)
            }
        }
    }

    /// Decode StellarMessage XDR from the TX stream.
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        let (ttl, data) = match StellarMessage::from_xdr(data, Limits::none()) {
            Ok(StellarMessage::Transaction(envelope)) => {
                let tx =
                    crate::xdr::canonical_transaction_xdr(envelope).map_err(to_invalid_data)?;
                return Ok(TxStreamMessage::Tx(TxMessage { tx, ttl: 0 }));
            }
            Ok(message) => return Self::decode_stellar_message(message),
            Err(err) => {
                if data.len() < TX_TTL_BYTES {
                    return Err(to_invalid_data(err));
                }
                let ttl = u32::from_be_bytes(data[..TX_TTL_BYTES].try_into().unwrap());
                (ttl, &data[TX_TTL_BYTES..])
            }
        };

        match StellarMessage::from_xdr(data, Limits::none()).map_err(to_invalid_data)? {
            StellarMessage::Transaction(envelope) => {
                let tx =
                    crate::xdr::canonical_transaction_xdr(envelope).map_err(to_invalid_data)?;
                Ok(TxStreamMessage::Tx(TxMessage { tx, ttl }))
            }
            other => Self::decode_stellar_message(other),
        }
    }

    fn decode_stellar_message(message: StellarMessage) -> io::Result<Self> {
        match message {
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
        let msg = TxStreamMessage::Tx(TxMessage {
            tx: tx_data.clone(),
            ttl: 7,
        });

        let encoded = msg.encode().unwrap();
        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        assert_eq!(msg, decoded);
    }

    #[test]
    fn test_tx_stream_message_legacy_tx_decodes_with_zero_ttl() {
        let tx_data = valid_transaction_xdr(1000, 2, 1);
        let encoded = crate::xdr::encode_transaction_message_from_xdr(&tx_data).unwrap();

        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        assert_eq!(
            decoded,
            TxStreamMessage::Tx(TxMessage {
                tx: tx_data,
                ttl: 0,
            })
        );
    }

    #[test]
    fn test_tx_stream_message_inv_batch() {
        let mut batch = InvBatch::new();
        batch.push(InvEntry {
            hash: [0x42; 32],
            fee_per_op: 500,
        });
        let msg = TxStreamMessage::InvBatch(batch.clone());

        let encoded = msg.encode().unwrap();
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
        let msg = TxStreamMessage::GetData(gd.clone());

        let encoded = msg.encode().unwrap();
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
