//! TX Buffer - stores transactions for GETDATA responses.
//!
//! When we receive/submit a TX, we store it here so we can respond to
//! GETDATA requests from peers who received our INV.

use lru::LruCache;
use std::num::NonZeroUsize;
use std::time::{Duration, Instant};

/// Default buffer capacity
pub const TX_BUFFER_CAPACITY: usize = 10_000;

/// Maximum time to keep a TX in the buffer
pub const TX_BUFFER_MAX_AGE: Duration = Duration::from_secs(60);

/// Entry in the TX buffer
#[derive(Debug, Clone)]
struct BufferEntry {
    /// Full transaction data
    data: Vec<u8>,
    /// When the TX was added
    added_at: Instant,
}

/// Stores transactions for responding to GETDATA requests
#[derive(Debug)]
pub struct TxBuffer {
    /// TX hash -> entry
    buffer: LruCache<[u8; 32], BufferEntry>,
    /// Maximum age before expiry
    max_age: Duration,
}

impl TxBuffer {
    pub fn new() -> Self {
        TxBuffer {
            buffer: LruCache::new(NonZeroUsize::new(TX_BUFFER_CAPACITY).unwrap()),
            max_age: TX_BUFFER_MAX_AGE,
        }
    }

    /// Store a TX in the buffer
    pub fn insert(&mut self, hash: [u8; 32], data: Vec<u8>) {
        self.buffer.put(
            hash,
            BufferEntry {
                data,
                added_at: Instant::now(),
            },
        );
    }

    /// Get a TX, cloning the data (avoids borrow issues)
    pub fn get_cloned(&mut self, hash: &[u8; 32]) -> Option<Vec<u8>> {
        let entry = self.buffer.get(hash)?;
        if entry.added_at.elapsed() > self.max_age {
            self.buffer.pop(hash);
            return None;
        }
        Some(entry.data.clone())
    }
}

impl Default for TxBuffer {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hash(n: u8) -> [u8; 32] {
        [n; 32]
    }

    #[test]
    fn test_tx_buffer_get_cloned() {
        let mut buffer = TxBuffer::new();
        let tx_data = vec![0x01, 0x02, 0x03];

        buffer.insert(hash(1), tx_data.clone());

        let cloned = buffer.get_cloned(&hash(1)).unwrap();
        assert_eq!(cloned, tx_data);
    }

    #[test]
    fn test_tx_buffer_not_found() {
        let mut buffer = TxBuffer::new();
        assert!(buffer.get_cloned(&hash(1)).is_none());
    }

    #[test]
    fn test_tx_buffer_overwrite() {
        let mut buffer = TxBuffer::new();

        buffer.insert(hash(1), vec![1, 1, 1]);
        buffer.insert(hash(1), vec![2, 2, 2]); // Overwrite

        let retrieved = buffer.get_cloned(&hash(1)).unwrap();
        assert_eq!(retrieved, vec![2, 2, 2]);
    }
}
