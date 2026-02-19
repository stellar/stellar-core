//! INV/GETDATA message types for bandwidth-efficient TX flooding.
//!
//! Wire format:
//! - All messages on TX stream now have a 1-byte type prefix
//! - TX (0x01): [tx_data] - full transaction (backward compat)
//! - INV_BATCH (0x02): [count:4][entries...] - batch of TX announcements
//! - GETDATA (0x03): [count:4][hashes...] - request for specific TXs

use std::io::{self, Read, Write};

/// Message type identifiers for TX stream
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TxMessageType {
    /// Full transaction data (legacy, also used for GETDATA response)
    Tx = 0x01,
    /// Batch of transaction inventory announcements
    InvBatch = 0x02,
    /// Request for specific transactions by hash
    GetData = 0x03,
}

impl TxMessageType {
    pub fn from_byte(b: u8) -> Option<Self> {
        match b {
            0x01 => Some(TxMessageType::Tx),
            0x02 => Some(TxMessageType::InvBatch),
            0x03 => Some(TxMessageType::GetData),
            _ => None,
        }
    }
}

/// A single INV entry: hash + fee for prioritization
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InvEntry {
    /// SHA256 hash of the transaction
    pub hash: [u8; 32],
    /// Fee per operation (for smart GETDATA prioritization)
    pub fee_per_op: i64,
}

impl InvEntry {
    pub const SIZE: usize = 32 + 8; // 40 bytes

    pub fn encode(&self, buf: &mut impl Write) -> io::Result<()> {
        buf.write_all(&self.hash)?;
        buf.write_all(&self.fee_per_op.to_be_bytes())?;
        Ok(())
    }

    pub fn decode(buf: &mut impl Read) -> io::Result<Self> {
        let mut hash = [0u8; 32];
        buf.read_exact(&mut hash)?;
        let mut fee_bytes = [0u8; 8];
        buf.read_exact(&mut fee_bytes)?;
        Ok(InvEntry {
            hash,
            fee_per_op: i64::from_be_bytes(fee_bytes),
        })
    }
}

/// Maximum entries in a single INV_BATCH message
pub const INV_BATCH_MAX_SIZE: usize = 1000;

/// Maximum hashes in a single GETDATA message
pub const GETDATA_MAX_SIZE: usize = 1000;

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

    /// Check if batch is at capacity
    pub fn is_full(&self) -> bool {
        self.entries.len() >= INV_BATCH_MAX_SIZE
    }

    /// Encode to wire format: [type:1][count:4][entries...]
    /// Note: type byte is written by caller in framed message
    pub fn encode(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(4 + self.entries.len() * InvEntry::SIZE);
        buf.extend_from_slice(&(self.entries.len() as u32).to_be_bytes());
        for entry in &self.entries {
            entry.encode(&mut buf).expect("Vec write never fails");
        }
        buf
    }

    /// Decode from wire format (after type byte has been read)
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 4 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "INV_BATCH too short for count",
            ));
        }

        let count = u32::from_be_bytes([data[0], data[1], data[2], data[3]]) as usize;

        if count > INV_BATCH_MAX_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "INV_BATCH count {} exceeds max {}",
                    count, INV_BATCH_MAX_SIZE
                ),
            ));
        }

        let expected_len = 4 + count * InvEntry::SIZE;
        if data.len() < expected_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "INV_BATCH data too short: {} < {}",
                    data.len(),
                    expected_len
                ),
            ));
        }

        let mut entries = Vec::with_capacity(count);
        let mut cursor = std::io::Cursor::new(&data[4..]);
        for _ in 0..count {
            entries.push(InvEntry::decode(&mut cursor)?);
        }

        Ok(InvBatch { entries })
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

    pub fn is_full(&self) -> bool {
        self.hashes.len() >= GETDATA_MAX_SIZE
    }

    /// Encode to wire format: [count:4][hashes...]
    pub fn encode(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(4 + self.hashes.len() * 32);
        buf.extend_from_slice(&(self.hashes.len() as u32).to_be_bytes());
        for hash in &self.hashes {
            buf.extend_from_slice(hash);
        }
        buf
    }

    /// Decode from wire format
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.len() < 4 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "GETDATA too short for count",
            ));
        }

        let count = u32::from_be_bytes([data[0], data[1], data[2], data[3]]) as usize;

        if count > GETDATA_MAX_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("GETDATA count {} exceeds max {}", count, GETDATA_MAX_SIZE),
            ));
        }

        let expected_len = 4 + count * 32;
        if data.len() < expected_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("GETDATA data too short: {} < {}", data.len(), expected_len),
            ));
        }

        let mut hashes = Vec::with_capacity(count);
        for i in 0..count {
            let start = 4 + i * 32;
            let mut hash = [0u8; 32];
            hash.copy_from_slice(&data[start..start + 32]);
            hashes.push(hash);
        }

        Ok(GetData { hashes })
    }
}

impl Default for GetData {
    fn default() -> Self {
        Self::new()
    }
}

/// Parsed TX stream message
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TxStreamMessage {
    /// Full transaction data
    Tx(Vec<u8>),
    /// Batch of INV announcements
    InvBatch(InvBatch),
    /// Request for transactions
    GetData(GetData),
}

impl TxStreamMessage {
    /// Encode message with type prefix
    pub fn encode(&self) -> Vec<u8> {
        match self {
            TxStreamMessage::Tx(data) => {
                let mut buf = Vec::with_capacity(1 + data.len());
                buf.push(TxMessageType::Tx as u8);
                buf.extend_from_slice(data);
                buf
            }
            TxStreamMessage::InvBatch(batch) => {
                let payload = batch.encode();
                let mut buf = Vec::with_capacity(1 + payload.len());
                buf.push(TxMessageType::InvBatch as u8);
                buf.extend_from_slice(&payload);
                buf
            }
            TxStreamMessage::GetData(gd) => {
                let payload = gd.encode();
                let mut buf = Vec::with_capacity(1 + payload.len());
                buf.push(TxMessageType::GetData as u8);
                buf.extend_from_slice(&payload);
                buf
            }
        }
    }

    /// Decode message (including type prefix)
    pub fn decode(data: &[u8]) -> io::Result<Self> {
        if data.is_empty() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "Empty message"));
        }

        let msg_type = TxMessageType::from_byte(data[0]).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unknown message type: 0x{:02x}", data[0]),
            )
        })?;

        let payload = &data[1..];

        match msg_type {
            TxMessageType::Tx => Ok(TxStreamMessage::Tx(payload.to_vec())),
            TxMessageType::InvBatch => Ok(TxStreamMessage::InvBatch(InvBatch::decode(payload)?)),
            TxMessageType::GetData => Ok(TxStreamMessage::GetData(GetData::decode(payload)?)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_inv_entry_encode_decode() {
        let entry = InvEntry {
            hash: [0x42; 32],
            fee_per_op: 12345,
        };

        let mut buf = Vec::new();
        entry.encode(&mut buf).unwrap();
        assert_eq!(buf.len(), InvEntry::SIZE);

        let decoded = InvEntry::decode(&mut std::io::Cursor::new(&buf)).unwrap();
        assert_eq!(entry, decoded);
    }

    #[test]
    fn test_inv_entry_negative_fee() {
        let entry = InvEntry {
            hash: [0xAB; 32],
            fee_per_op: -999,
        };

        let mut buf = Vec::new();
        entry.encode(&mut buf).unwrap();

        let decoded = InvEntry::decode(&mut std::io::Cursor::new(&buf)).unwrap();
        assert_eq!(entry.fee_per_op, decoded.fee_per_op);
    }

    #[test]
    fn test_inv_batch_encode_decode() {
        let mut batch = InvBatch::new();
        batch.push(InvEntry {
            hash: [0x01; 32],
            fee_per_op: 100,
        });
        batch.push(InvEntry {
            hash: [0x02; 32],
            fee_per_op: 200,
        });

        let encoded = batch.encode();
        assert_eq!(encoded.len(), 4 + 2 * InvEntry::SIZE);

        let decoded = InvBatch::decode(&encoded).unwrap();
        assert_eq!(batch, decoded);
    }

    #[test]
    fn test_inv_batch_empty() {
        let batch = InvBatch::new();
        let encoded = batch.encode();
        assert_eq!(encoded.len(), 4); // Just the count

        let decoded = InvBatch::decode(&encoded).unwrap();
        assert!(decoded.entries.is_empty());
    }

    #[test]
    fn test_inv_batch_max_size() {
        let mut batch = InvBatch::new();
        for i in 0..INV_BATCH_MAX_SIZE {
            batch.push(InvEntry {
                hash: [(i % 256) as u8; 32],
                fee_per_op: i as i64,
            });
        }

        assert!(batch.is_full());
        let encoded = batch.encode();
        assert_eq!(encoded.len(), 4 + INV_BATCH_MAX_SIZE * InvEntry::SIZE);

        let decoded = InvBatch::decode(&encoded).unwrap();
        assert_eq!(decoded.entries.len(), INV_BATCH_MAX_SIZE);
    }

    #[test]
    fn test_inv_batch_oversized_rejected() {
        // Manually craft an oversized batch
        let mut data = vec![0u8; 4 + 1001 * InvEntry::SIZE];
        data[0..4].copy_from_slice(&1001u32.to_be_bytes());

        let result = InvBatch::decode(&data);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds max"));
    }

    #[test]
    fn test_getdata_encode_decode() {
        let mut gd = GetData::new();
        gd.push([0xAA; 32]);
        gd.push([0xBB; 32]);
        gd.push([0xCC; 32]);

        let encoded = gd.encode();
        assert_eq!(encoded.len(), 4 + 3 * 32);

        let decoded = GetData::decode(&encoded).unwrap();
        assert_eq!(gd, decoded);
    }

    #[test]
    fn test_getdata_empty() {
        let gd = GetData::new();
        let encoded = gd.encode();
        assert_eq!(encoded.len(), 4);

        let decoded = GetData::decode(&encoded).unwrap();
        assert!(decoded.hashes.is_empty());
    }

    #[test]
    fn test_getdata_max_size() {
        let mut gd = GetData::new();
        for i in 0..GETDATA_MAX_SIZE {
            gd.push([(i % 256) as u8; 32]);
        }

        assert!(gd.is_full());
        let encoded = gd.encode();
        assert_eq!(encoded.len(), 4 + GETDATA_MAX_SIZE * 32);

        let decoded = GetData::decode(&encoded).unwrap();
        assert_eq!(decoded.hashes.len(), GETDATA_MAX_SIZE);
    }

    #[test]
    fn test_getdata_oversized_rejected() {
        let mut data = vec![0u8; 4 + 1001 * 32];
        data[0..4].copy_from_slice(&1001u32.to_be_bytes());

        let result = GetData::decode(&data);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds max"));
    }

    #[test]
    fn test_tx_stream_message_tx() {
        let tx_data = vec![0x01, 0x02, 0x03, 0x04];
        let msg = TxStreamMessage::Tx(tx_data.clone());

        let encoded = msg.encode();
        assert_eq!(encoded[0], TxMessageType::Tx as u8);
        assert_eq!(&encoded[1..], &tx_data);

        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        assert_eq!(msg, decoded);
    }

    #[test]
    fn test_tx_stream_message_inv_batch() {
        let mut batch = InvBatch::new();
        batch.push(InvEntry {
            hash: [0x42; 32],
            fee_per_op: 500,
        });
        let msg = TxStreamMessage::InvBatch(batch.clone());

        let encoded = msg.encode();
        assert_eq!(encoded[0], TxMessageType::InvBatch as u8);

        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        if let TxStreamMessage::InvBatch(decoded_batch) = decoded {
            assert_eq!(batch, decoded_batch);
        } else {
            panic!("Expected InvBatch");
        }
    }

    #[test]
    fn test_tx_stream_message_getdata() {
        let mut gd = GetData::new();
        gd.push([0xFF; 32]);
        let msg = TxStreamMessage::GetData(gd.clone());

        let encoded = msg.encode();
        assert_eq!(encoded[0], TxMessageType::GetData as u8);

        let decoded = TxStreamMessage::decode(&encoded).unwrap();
        if let TxStreamMessage::GetData(decoded_gd) = decoded {
            assert_eq!(gd, decoded_gd);
        } else {
            panic!("Expected GetData");
        }
    }

    #[test]
    fn test_message_type_from_byte() {
        assert_eq!(TxMessageType::from_byte(0x01), Some(TxMessageType::Tx));
        assert_eq!(
            TxMessageType::from_byte(0x02),
            Some(TxMessageType::InvBatch)
        );
        assert_eq!(TxMessageType::from_byte(0x03), Some(TxMessageType::GetData));
        assert_eq!(TxMessageType::from_byte(0x00), None);
        assert_eq!(TxMessageType::from_byte(0xFF), None);
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
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("Unknown message type"));
    }

    #[test]
    fn test_inv_batch_truncated_fails() {
        // Count says 2 entries but only provide data for 1
        let mut data = vec![0u8; 4 + InvEntry::SIZE];
        data[0..4].copy_from_slice(&2u32.to_be_bytes());

        let result = InvBatch::decode(&data);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("too short"));
    }

    #[test]
    fn test_getdata_truncated_fails() {
        // Count says 3 hashes but only provide 2
        let mut data = vec![0u8; 4 + 2 * 32];
        data[0..4].copy_from_slice(&3u32.to_be_bytes());

        let result = GetData::decode(&data);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("too short"));
    }
}
