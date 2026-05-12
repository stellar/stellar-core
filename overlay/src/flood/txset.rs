//! TX set cache for peer request/response handling.

use std::collections::HashMap;

/// 32-byte hash
pub type Hash256 = [u8; 32];

/// A cached TX set with its XDR and hash.
#[derive(Debug, Clone)]
pub struct CachedTxSet {
    /// The TX set hash (SHA256 of XDR)
    pub hash: Hash256,
    /// The serialized GeneralizedTransactionSet XDR
    pub xdr: Vec<u8>,
    /// Ledger sequence this was built for
    pub ledger_seq: u32,
}

/// TX set cache - stores built TX sets by hash for retrieval.
pub struct TxSetCache {
    /// TX sets by hash
    by_hash: HashMap<Hash256, CachedTxSet>,
    /// Max cache size
    max_size: usize,
}

impl TxSetCache {
    pub fn new(max_size: usize) -> Self {
        Self {
            by_hash: HashMap::new(),
            max_size,
        }
    }

    /// Insert a TX set into the cache.
    pub fn insert(&mut self, tx_set: CachedTxSet) {
        if self.by_hash.len() >= self.max_size {
            // Evict oldest (simple strategy - just remove one)
            if let Some(&hash) = self.by_hash.keys().next() {
                self.by_hash.remove(&hash);
            }
        }
        self.by_hash.insert(tx_set.hash, tx_set);
    }

    /// Get a TX set by hash.
    pub fn get(&self, hash: &Hash256) -> Option<&CachedTxSet> {
        self.by_hash.get(hash)
    }

    /// Remove TX sets for ledgers before the given sequence.
    pub fn evict_before(&mut self, ledger_seq: u32) {
        self.by_hash.retain(|_, v| v.ledger_seq >= ledger_seq);
    }

    /// Get number of cached TX sets.
    pub fn len(&self) -> usize {
        self.by_hash.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cache_insert_and_get() {
        let mut cache = TxSetCache::new(10);

        let tx_set = CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![1, 2, 3],
            ledger_seq: 100,
        };

        cache.insert(tx_set.clone());

        let retrieved = cache.get(&[1u8; 32]);
        assert!(retrieved.is_some());
        let retrieved = retrieved.unwrap();
        assert_eq!(retrieved.ledger_seq, 100);
        assert_eq!(retrieved.xdr, vec![1, 2, 3]);
        assert!(cache.get(&[2u8; 32]).is_none());
    }

    #[test]
    fn test_cache_evict_before() {
        let mut cache = TxSetCache::new(10);

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![],
            ledger_seq: 100,
        });
        cache.insert(CachedTxSet {
            hash: [2u8; 32],
            xdr: vec![],
            ledger_seq: 200,
        });

        cache.evict_before(150);

        assert!(cache.get(&[1u8; 32]).is_none()); // evicted
        assert!(cache.get(&[2u8; 32]).is_some()); // kept
    }

    #[test]
    fn test_cache_capacity_eviction() {
        let mut cache = TxSetCache::new(2); // Small cache

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![],
            ledger_seq: 100,
        });
        cache.insert(CachedTxSet {
            hash: [2u8; 32],
            xdr: vec![],
            ledger_seq: 101,
        });

        assert_eq!(cache.len(), 2);

        // Insert 3rd - should evict one
        cache.insert(CachedTxSet {
            hash: [3u8; 32],
            xdr: vec![],
            ledger_seq: 102,
        });

        assert_eq!(cache.len(), 2, "Cache should stay at capacity");
        assert!(
            cache.get(&[3u8; 32]).is_some(),
            "New item should be present"
        );
    }

    #[test]
    fn test_cache_overwrite_same_hash() {
        let mut cache = TxSetCache::new(10);

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![1, 2, 3],
            ledger_seq: 100,
        });

        // Insert with same hash but different data
        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![4, 5, 6],
            ledger_seq: 200,
        });

        assert_eq!(cache.len(), 1, "Should not create duplicate");
        let retrieved = cache.get(&[1u8; 32]).unwrap();
        assert_eq!(retrieved.ledger_seq, 200, "Should have newer data");
        assert_eq!(retrieved.xdr, vec![4, 5, 6]);
    }
}
