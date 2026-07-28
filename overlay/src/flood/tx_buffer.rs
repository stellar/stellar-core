//! TX Buffer - stores transactions for GETDATA responses.
//!
//! When we receive/submit a TX, we store it here so we can respond to
//! GETDATA requests from peers who received our INV.

use crate::wire::ValidatedTx;
use lru::LruCache;
use std::num::NonZeroUsize;
use std::sync::Arc;
use std::time::{Duration, Instant};

/// Default buffer capacity
pub const TX_BUFFER_CAPACITY: usize = 10_000;

/// Maximum time to keep a TX in the buffer
pub const TX_BUFFER_MAX_AGE: Duration = Duration::from_secs(60);

/// Entry in the TX buffer
#[derive(Debug)]
struct BufferEntry {
    /// The transaction, shared with the mempool and in-flight sends
    tx: Arc<ValidatedTx>,
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

    /// Store a TX in the buffer, keyed by its hash
    pub fn insert(&mut self, tx: Arc<ValidatedTx>) {
        self.buffer.put(
            *tx.hash(),
            BufferEntry {
                tx,
                added_at: Instant::now(),
            },
        );
    }

    /// Get a TX; the returned `Arc` shares the buffered bytes without copying
    pub fn get(&mut self, hash: &[u8; 32]) -> Option<Arc<ValidatedTx>> {
        let entry = self.buffer.get(hash)?;
        if entry.added_at.elapsed() > self.max_age {
            self.buffer.pop(hash);
            return None;
        }
        Some(Arc::clone(&entry.tx))
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
    use crate::xdr::tests::valid_transaction_xdr;

    /// Build a validated tx with a distinct hash per `seq`.
    fn make_tx(fee: u32, seq: i64) -> Arc<ValidatedTx> {
        let bytes = valid_transaction_xdr(fee, seq, 1);
        ValidatedTx::from_core_trusted(bytes, fee as i64, 1).unwrap()
    }

    #[test]
    fn test_tx_buffer_get() {
        let mut buffer = TxBuffer::new();
        let tx = make_tx(100, 1);
        let hash = *tx.hash();

        buffer.insert(Arc::clone(&tx));

        let retrieved = buffer.get(&hash).unwrap();
        assert_eq!(retrieved.bytes(), tx.bytes());
        // Shared, not copied: both handles point at the same allocation
        assert!(std::ptr::eq(retrieved.bytes(), tx.bytes()));
    }

    #[test]
    fn test_tx_buffer_not_found() {
        let mut buffer = TxBuffer::new();
        assert!(buffer.get(&[1u8; 32]).is_none());
    }

    #[test]
    fn test_tx_buffer_overwrite() {
        let mut buffer = TxBuffer::new();
        let tx = make_tx(100, 1);
        let hash = *tx.hash();

        buffer.insert(Arc::clone(&tx));
        buffer.insert(Arc::clone(&tx)); // Overwrite with same hash

        let retrieved = buffer.get(&hash).unwrap();
        assert_eq!(retrieved.bytes(), tx.bytes());
    }
}
