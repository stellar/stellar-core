//! TX set cache for peer request/response handling.

use std::collections::{HashMap, VecDeque};

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
    /// Order of hashes for eviction
    order: VecDeque<Hash256>,
    /// Max cache size
    max_size: usize,
}

impl TxSetCache {
    pub fn new(max_size: usize) -> Self {
        Self {
            by_hash: HashMap::new(),
            order: VecDeque::with_capacity(max_size),
            max_size,
        }
    }

    /// Insert a TX set into the cache.
    pub fn insert(&mut self, tx_set: CachedTxSet) {
        let hash = tx_set.hash;
        if self.by_hash.insert(tx_set.hash, tx_set).is_none() {
            if self.by_hash.len() > self.max_size {
                // Evict oldest; technically, this makes the minimum cache size 1, but that's fine
                // for our use case
                if let Some(to_evict) = self.order.pop_front() {
                    self.by_hash.remove(&to_evict);
                }
            }
            self.order.push_back(hash);
        }
    }

    /// Get a TX set by hash.
    pub fn get(&self, hash: &Hash256) -> Option<&CachedTxSet> {
        self.by_hash.get(hash)
    }

    /// Remove TX sets for ledgers before the given sequence.
    pub fn evict_before(&mut self, ledger_seq: u32) {
        self.order.retain(|hash| {
            if let Some(tx_set) = self.by_hash.get(hash) {
                tx_set.ledger_seq >= ledger_seq
            } else {
                false
            }
        });
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
            cache.get(&[1u8; 32]).is_none(),
            "Oldest inserted item should be evicted"
        );
        assert!(
            cache.get(&[2u8; 32]).is_some(),
            "Second-oldest item should remain"
        );
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

    #[test]
    fn test_cache_overwrite_when_full_keeps_size() {
        let mut cache = TxSetCache::new(1); // Cache holds exactly one entry

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![1, 2, 3],
            ledger_seq: 100,
        });
        assert_eq!(cache.len(), 1, "Cache is full");

        // Overwrite the only (existing) hash while the cache is full - must
        // not evict the entry or shrink the cache below capacity.
        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![4, 5, 6],
            ledger_seq: 102,
        });

        assert_eq!(cache.len(), 1, "Overwrite should not shrink the cache");
        let retrieved = cache
            .get(&[1u8; 32])
            .expect("Overwritten item should remain present");
        assert_eq!(retrieved.ledger_seq, 102, "Should have newer data");
        assert_eq!(retrieved.xdr, vec![4, 5, 6]);
    }

    #[test]
    fn test_overwrite_then_evict_before_keeps_order_in_sync() {
        let mut cache = TxSetCache::new(2);

        // Insert, then overwrite the same hash with a newer ledger_seq.
        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![1, 2, 3],
            ledger_seq: 100,
        });
        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![4, 5, 6],
            ledger_seq: 200,
        });

        // The entry's effective ledger_seq is now 200, so evicting before 150
        // must NOT remove it.
        cache.evict_before(150);
        assert!(
            cache.get(&[1u8; 32]).is_some(),
            "Overwritten entry (seq 200) must survive evict_before(150)"
        );
        assert_eq!(cache.len(), 1);

        // `order` must still track the surviving entry: filling the cache and
        // overflowing it should evict hash [1u8; 32] as the oldest, leaving the
        // cache exactly at capacity (not one over).
        cache.insert(CachedTxSet {
            hash: [2u8; 32],
            xdr: vec![],
            ledger_seq: 300,
        });
        cache.insert(CachedTxSet {
            hash: [3u8; 32],
            xdr: vec![],
            ledger_seq: 400,
        });

        assert_eq!(
            cache.len(),
            2,
            "Cache must stay at capacity; eviction tracking must not have leaked"
        );
        assert!(
            cache.get(&[1u8; 32]).is_none(),
            "Oldest entry should have been evicted by the capacity path"
        );
    }
}
