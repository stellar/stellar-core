//! TX Set builder for nomination.
//!
//! Builds GeneralizedTransactionSet XDR from mempool transactions.
//! Uses CLASSIC phase only for MVP. TODO: Add SOROBAN phase support.

use sha2::{Digest, Sha256};
use std::collections::HashMap;

/// 32-byte hash
pub type Hash256 = [u8; 32];

/// TX hash type (for mempool lookup)
pub type TxHash = [u8; 32];

/// A cached TX set with its XDR and hash.
#[derive(Debug, Clone)]
pub struct CachedTxSet {
    /// The TX set hash (SHA256 of XDR)
    pub hash: Hash256,
    /// The serialized GeneralizedTransactionSet XDR
    pub xdr: Vec<u8>,
    /// Ledger sequence this was built for
    pub ledger_seq: u32,
    /// Hashes of TXs included in this set (for mempool cleanup)
    pub tx_hashes: Vec<TxHash>,
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

    /// Remove a TX set by hash and return the TX hashes it contained.
    pub fn remove(&mut self, hash: &Hash256) -> Option<Vec<TxHash>> {
        self.by_hash.remove(hash).map(|ts| ts.tx_hashes)
    }

    /// Remove TX sets for ledgers before the given sequence.
    pub fn evict_before(&mut self, ledger_seq: u32) {
        self.by_hash.retain(|_, v| v.ledger_seq >= ledger_seq);
    }

    /// Clear all cached TX sets.
    pub fn clear(&mut self) {
        self.by_hash.clear();
    }

    /// Get number of cached TX sets.
    pub fn len(&self) -> usize {
        self.by_hash.len()
    }
}

/// Build a GeneralizedTransactionSet XDR from transaction envelopes.
///
/// Format (v1, CLASSIC sequential + SOROBAN parallel phases):
/// Protocol >= 23 requires Soroban phase to use parallel format (v=1).
/// ```text
/// GeneralizedTransactionSet {
///   v: 1
///   v1TxSet: TransactionSetV1 {
///     previousLedgerHash: Hash
///     phases: [TransactionPhase] {
///       [0]: TransactionPhase::v0Components (CLASSIC, sequential) {
///         [TxSetComponent {
///           type: TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE (0)
///           txsMaybeDiscountedFee: {
///             baseFee: null (no discount)
///             txs: [TransactionEnvelope]
///           }
///         }]
///       }
///       [1]: TransactionPhase::parallelTxsComponent (SOROBAN, parallel, empty) {
///         baseFee: null
///         executionStages: []
///       }
///     }
///   }
/// }
/// ```
pub fn build_tx_set_xdr(prev_ledger_hash: &Hash256, tx_envelopes: &[Vec<u8>]) -> Vec<u8> {
    let mut xdr = Vec::new();

    // GeneralizedTransactionSet union discriminant: v = 1 (4 bytes, big-endian)
    xdr.extend_from_slice(&1u32.to_be_bytes());

    // TransactionSetV1.previousLedgerHash (32 bytes)
    xdr.extend_from_slice(prev_ledger_hash);

    // TransactionSetV1.phases (xdr::xvector<TransactionPhase>)
    // Length = 2 (CLASSIC + SOROBAN phases - both required by validation)
    xdr.extend_from_slice(&2u32.to_be_bytes());

    // === PHASE 0: CLASSIC ===
    // TransactionPhase union discriminant: v = 0 (v0Components)
    xdr.extend_from_slice(&0u32.to_be_bytes());

    if tx_envelopes.is_empty() {
        // Empty phase: 0 components
        // Note: Empty components are rejected by validateSequentialPhaseXDRStructure
        xdr.extend_from_slice(&0u32.to_be_bytes());
    } else {
        // v0Components: xdr::xvector<TxSetComponent>
        // Length = 1 (single component with all txs, no discount)
        xdr.extend_from_slice(&1u32.to_be_bytes());

        // TxSetComponent union discriminant: TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE = 0
        xdr.extend_from_slice(&0u32.to_be_bytes());

        // txsMaybeDiscountedFee.baseFee: optional<int64>
        // 0 = not present (no discount)
        xdr.extend_from_slice(&0u32.to_be_bytes());

        // txsMaybeDiscountedFee.txs: xdr::xvector<TransactionEnvelope>
        // Length = number of transactions
        xdr.extend_from_slice(&(tx_envelopes.len() as u32).to_be_bytes());

        // Append each transaction envelope
        for tx in tx_envelopes {
            xdr.extend_from_slice(tx);
        }
    }

    // === PHASE 1: SOROBAN (empty, parallel format) ===
    // Protocol >= 23 requires parallelTxsComponent (v=1) for Soroban phase
    // TransactionPhase union discriminant: v = 1 (parallelTxsComponent)
    xdr.extend_from_slice(&1u32.to_be_bytes());

    // ParallelTxsComponent.baseFee: optional<int64>
    // 0 = not present (no discount)
    xdr.extend_from_slice(&0u32.to_be_bytes());

    // ParallelTxsComponent.executionStages: xvector<ParallelTxExecutionStage>
    // Length = 0 (no Soroban transactions)
    xdr.extend_from_slice(&0u32.to_be_bytes());

    xdr
}

/// Compute the hash of a TX set XDR.
pub fn hash_tx_set(xdr: &[u8]) -> Hash256 {
    let mut hasher = Sha256::new();
    hasher.update(xdr);
    let result = hasher.finalize();
    let mut hash = [0u8; 32];
    hash.copy_from_slice(&result);
    hash
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_build_empty_tx_set() {
        let prev_hash = [1u8; 32];
        let xdr = build_tx_set_xdr(&prev_hash, &[]);

        // Check structure for empty tx set (no components when empty):
        // [0..4]: v = 1
        assert_eq!(&xdr[0..4], &1u32.to_be_bytes());
        // [4..36]: previousLedgerHash
        assert_eq!(&xdr[4..36], &prev_hash);
        // [36..40]: phases length = 2 (CLASSIC + SOROBAN)
        assert_eq!(&xdr[36..40], &2u32.to_be_bytes());
        // [40..44]: phase 0 discriminant = 0 (v0Components for CLASSIC)
        assert_eq!(&xdr[40..44], &0u32.to_be_bytes());
        // [44..48]: phase 0 components length = 0 (empty CLASSIC)
        assert_eq!(&xdr[44..48], &0u32.to_be_bytes());
        // [48..52]: phase 1 discriminant = 1 (parallelTxsComponent for SOROBAN, protocol >= 23)
        assert_eq!(&xdr[48..52], &1u32.to_be_bytes());
        // [52..56]: phase 1 baseFee = 0 (not present)
        assert_eq!(&xdr[52..56], &0u32.to_be_bytes());
        // [56..60]: phase 1 executionStages length = 0 (empty SOROBAN)
        assert_eq!(&xdr[56..60], &0u32.to_be_bytes());
    }

    #[test]
    fn test_build_tx_set_with_txs() {
        let prev_hash = [2u8; 32];
        let tx1 = vec![0xAA, 0xBB, 0xCC];
        let tx2 = vec![0xDD, 0xEE];

        let xdr = build_tx_set_xdr(&prev_hash, &[tx1.clone(), tx2.clone()]);

        // Structure with TXs:
        // [0..4]: v = 1
        // [4..36]: prev_hash
        // [36..40]: phases len = 2
        // [40..44]: phase 0 discriminant = 0 (v0Components)
        // [44..48]: components len = 1
        // [48..52]: component discriminant = 0 (TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE)
        // [52..56]: baseFee = 0 (not present)
        // [56..60]: txs len = 2
        assert_eq!(&xdr[56..60], &2u32.to_be_bytes());

        // TXs are appended raw (the test txs have no length prefix in this simplified format)
        // [60..63]: tx1 (3 bytes)
        assert_eq!(&xdr[60..63], &tx1[..]);
        // [63..65]: tx2 (2 bytes)
        assert_eq!(&xdr[63..65], &tx2[..]);

        // SOROBAN phase follows with parallel format (v=1):
        // [65..69]: phase 1 discriminant = 1 (parallelTxsComponent)
        assert_eq!(&xdr[65..69], &1u32.to_be_bytes());
        // [69..73]: baseFee = 0 (not present)
        assert_eq!(&xdr[69..73], &0u32.to_be_bytes());
        // [73..77]: executionStages len = 0
        assert_eq!(&xdr[73..77], &0u32.to_be_bytes());
    }

    #[test]
    fn test_hash_deterministic() {
        let prev_hash = [3u8; 32];
        let xdr = build_tx_set_xdr(&prev_hash, &[]);

        let hash1 = hash_tx_set(&xdr);
        let hash2 = hash_tx_set(&xdr);

        assert_eq!(hash1, hash2);
    }

    #[test]
    fn test_hash_different_for_different_content() {
        let prev_hash = [3u8; 32];
        let xdr1 = build_tx_set_xdr(&prev_hash, &[]);
        let xdr2 = build_tx_set_xdr(&prev_hash, &[vec![1, 2, 3]]);

        let hash1 = hash_tx_set(&xdr1);
        let hash2 = hash_tx_set(&xdr2);

        assert_ne!(
            hash1, hash2,
            "Different TX sets should have different hashes"
        );
    }

    #[test]
    fn test_hash_different_for_different_prev_hash() {
        let xdr1 = build_tx_set_xdr(&[1u8; 32], &[]);
        let xdr2 = build_tx_set_xdr(&[2u8; 32], &[]);

        let hash1 = hash_tx_set(&xdr1);
        let hash2 = hash_tx_set(&xdr2);

        assert_ne!(
            hash1, hash2,
            "TX sets with different prev_hash should have different hashes"
        );
    }

    #[test]
    fn test_cache_insert_and_get() {
        let mut cache = TxSetCache::new(10);

        let tx_set = CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![1, 2, 3],
            ledger_seq: 100,
            tx_hashes: vec![],
        };

        cache.insert(tx_set.clone());

        let retrieved = cache.get(&[1u8; 32]);
        assert!(retrieved.is_some());
        assert_eq!(retrieved.unwrap().ledger_seq, 100);
    }

    #[test]
    fn test_cache_evict_before() {
        let mut cache = TxSetCache::new(10);

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![],
            ledger_seq: 100,
            tx_hashes: vec![],
        });
        cache.insert(CachedTxSet {
            hash: [2u8; 32],
            xdr: vec![],
            ledger_seq: 200,
            tx_hashes: vec![],
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
            tx_hashes: vec![],
        });
        cache.insert(CachedTxSet {
            hash: [2u8; 32],
            xdr: vec![],
            ledger_seq: 101,
            tx_hashes: vec![],
        });

        assert_eq!(cache.len(), 2);

        // Insert 3rd - should evict one
        cache.insert(CachedTxSet {
            hash: [3u8; 32],
            xdr: vec![],
            ledger_seq: 102,
            tx_hashes: vec![],
        });

        assert_eq!(cache.len(), 2, "Cache should stay at capacity");
        assert!(
            cache.get(&[3u8; 32]).is_some(),
            "New item should be present"
        );
    }

    #[test]
    fn test_cache_remove_returns_tx_hashes() {
        let mut cache = TxSetCache::new(10);

        let tx_hashes = vec![[0xAA; 32], [0xBB; 32]];
        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![],
            ledger_seq: 100,
            tx_hashes: tx_hashes.clone(),
        });

        let removed = cache.remove(&[1u8; 32]);
        assert!(removed.is_some());
        assert_eq!(removed.unwrap(), tx_hashes);

        // Should be gone now
        assert!(cache.get(&[1u8; 32]).is_none());
    }

    #[test]
    fn test_cache_remove_nonexistent() {
        let mut cache = TxSetCache::new(10);

        let removed = cache.remove(&[99u8; 32]);
        assert!(removed.is_none());
    }

    #[test]
    fn test_cache_clear() {
        let mut cache = TxSetCache::new(10);

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![],
            ledger_seq: 100,
            tx_hashes: vec![],
        });
        cache.insert(CachedTxSet {
            hash: [2u8; 32],
            xdr: vec![],
            ledger_seq: 101,
            tx_hashes: vec![],
        });

        assert_eq!(cache.len(), 2);

        cache.clear();

        assert_eq!(cache.len(), 0);
        assert!(cache.get(&[1u8; 32]).is_none());
        assert!(cache.get(&[2u8; 32]).is_none());
    }

    #[test]
    fn test_cache_overwrite_same_hash() {
        let mut cache = TxSetCache::new(10);

        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![1, 2, 3],
            ledger_seq: 100,
            tx_hashes: vec![],
        });

        // Insert with same hash but different data
        cache.insert(CachedTxSet {
            hash: [1u8; 32],
            xdr: vec![4, 5, 6],
            ledger_seq: 200,
            tx_hashes: vec![],
        });

        assert_eq!(cache.len(), 1, "Should not create duplicate");
        let retrieved = cache.get(&[1u8; 32]).unwrap();
        assert_eq!(retrieved.ledger_seq, 200, "Should have newer data");
        assert_eq!(retrieved.xdr, vec![4, 5, 6]);
    }
}
