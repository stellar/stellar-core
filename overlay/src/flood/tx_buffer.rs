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
        Self::with_config(TX_BUFFER_CAPACITY, TX_BUFFER_MAX_AGE)
    }

    pub fn with_config(capacity: usize, max_age: Duration) -> Self {
        TxBuffer {
            buffer: LruCache::new(NonZeroUsize::new(capacity).unwrap()),
            max_age,
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

    /// Get a TX from the buffer (returns None if expired)
    pub fn get(&mut self, hash: &[u8; 32]) -> Option<&[u8]> {
        // Check if expired
        let entry = self.buffer.get(hash)?;
        if entry.added_at.elapsed() > self.max_age {
            // Expired - remove and return None
            self.buffer.pop(hash);
            return None;
        }

        // Re-get to return reference (borrow checker)
        self.buffer.get(hash).map(|e| e.data.as_slice())
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

    /// Check if a TX is in the buffer (and not expired)
    pub fn contains(&mut self, hash: &[u8; 32]) -> bool {
        self.get(hash).is_some()
    }

    /// Remove a TX from the buffer
    pub fn remove(&mut self, hash: &[u8; 32]) -> Option<Vec<u8>> {
        self.buffer.pop(hash).map(|e| e.data)
    }

    /// Remove expired entries (call periodically for cleanup)
    pub fn evict_expired(&mut self) -> usize {
        let expired: Vec<_> = self
            .buffer
            .iter()
            .filter(|(_, entry)| entry.added_at.elapsed() > self.max_age)
            .map(|(hash, _)| *hash)
            .collect();

        let count = expired.len();
        for hash in expired {
            self.buffer.pop(&hash);
        }
        count
    }

    /// Number of TXs in the buffer
    pub fn len(&self) -> usize {
        self.buffer.len()
    }

    pub fn is_empty(&self) -> bool {
        self.buffer.is_empty()
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
    fn test_tx_buffer_insert_get() {
        let mut buffer = TxBuffer::new();
        let tx_data = vec![0x01, 0x02, 0x03];

        buffer.insert(hash(1), tx_data.clone());

        let retrieved = buffer.get(&hash(1)).unwrap();
        assert_eq!(retrieved, &tx_data);
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
        assert!(buffer.get(&hash(1)).is_none());
    }

    #[test]
    fn test_tx_buffer_expiry() {
        let mut buffer = TxBuffer::with_config(100, Duration::from_millis(50));
        let tx_data = vec![0x01, 0x02, 0x03];

        buffer.insert(hash(1), tx_data);

        // Should be available immediately
        assert!(buffer.get(&hash(1)).is_some());

        // Wait for expiry
        std::thread::sleep(Duration::from_millis(60));

        // Should be expired now
        assert!(buffer.get(&hash(1)).is_none());
    }

    #[test]
    fn test_tx_buffer_contains() {
        let mut buffer = TxBuffer::new();

        assert!(!buffer.contains(&hash(1)));

        buffer.insert(hash(1), vec![0x01]);
        assert!(buffer.contains(&hash(1)));
    }

    #[test]
    fn test_tx_buffer_remove() {
        let mut buffer = TxBuffer::new();
        let tx_data = vec![0x01, 0x02, 0x03];

        buffer.insert(hash(1), tx_data.clone());

        let removed = buffer.remove(&hash(1)).unwrap();
        assert_eq!(removed, tx_data);
        assert!(!buffer.contains(&hash(1)));
    }

    #[test]
    fn test_tx_buffer_lru_eviction() {
        let mut buffer = TxBuffer::with_config(3, Duration::from_secs(60));

        buffer.insert(hash(1), vec![1]);
        buffer.insert(hash(2), vec![2]);
        buffer.insert(hash(3), vec![3]);

        assert_eq!(buffer.len(), 3);

        // Adding 4th should evict oldest (hash(1))
        buffer.insert(hash(4), vec![4]);

        assert_eq!(buffer.len(), 3);
        assert!(buffer.get(&hash(1)).is_none()); // Evicted
        assert!(buffer.get(&hash(4)).is_some()); // New one present
    }

    #[test]
    fn test_tx_buffer_evict_expired() {
        let mut buffer = TxBuffer::with_config(100, Duration::from_millis(50));

        buffer.insert(hash(1), vec![1]);
        buffer.insert(hash(2), vec![2]);

        // None expired yet
        assert_eq!(buffer.evict_expired(), 0);

        // Wait for expiry
        std::thread::sleep(Duration::from_millis(60));

        // Both should be evicted
        assert_eq!(buffer.evict_expired(), 2);
        assert!(buffer.is_empty());
    }

    #[test]
    fn test_tx_buffer_overwrite() {
        let mut buffer = TxBuffer::new();

        buffer.insert(hash(1), vec![1, 1, 1]);
        buffer.insert(hash(1), vec![2, 2, 2]); // Overwrite

        let retrieved = buffer.get(&hash(1)).unwrap();
        assert_eq!(retrieved, &[2, 2, 2]);
    }

    #[test]
    fn test_tx_buffer_len() {
        let mut buffer = TxBuffer::new();

        assert!(buffer.is_empty());
        assert_eq!(buffer.len(), 0);

        buffer.insert(hash(1), vec![1]);
        buffer.insert(hash(2), vec![2]);

        assert!(!buffer.is_empty());
        assert_eq!(buffer.len(), 2);
    }
}
