//! Transaction mempool.
//!
//! Stores transactions waiting to be included in the ledger, indexed for:
//! - Deduplication by hash
//! - Fee-based ordering for nomination

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tracing::trace;

use crate::wire::ValidatedTx;

/// 32-byte transaction hash
pub type TxHash = [u8; 32];

/// A mempool-resident transaction: the shared validated tx plus its arrival
/// time (for age-based eviction). Internal detail — callers get the shared
/// `Arc<ValidatedTx>` back from [`Mempool::get`].
#[derive(Debug, Clone)]
struct MempoolEntry {
    meta: Arc<ValidatedTx>,
    received_at: Instant,
}

/// Comparison key for fee-sorted ordering (higher fee = higher priority)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FeePriority {
    /// Fee (higher is better)
    fee: u64,
    /// Number of ops (lower is better for same fee)
    num_ops: u32,
    /// Hash for tie-breaking
    hash: TxHash,
}

impl Ord for FeePriority {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        // Higher fee per op = higher priority
        // fee1/ops1 > fee2/ops2 iff fee1*ops2 > fee2*ops1
        let left = self.fee * (other.num_ops as u64);
        let right = other.fee * (self.num_ops as u64);

        match left.cmp(&right).reverse() {
            // reverse for descending order
            std::cmp::Ordering::Equal => {
                // Same fee/op ratio: prefer fewer ops (simpler tx)
                match self.num_ops.cmp(&other.num_ops) {
                    std::cmp::Ordering::Equal => {
                        // Same ops: use hash for deterministic ordering
                        self.hash.cmp(&other.hash)
                    }
                    other => other,
                }
            }
            other => other,
        }
    }
}

impl PartialOrd for FeePriority {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl FeePriority {
    fn of(entry: &MempoolEntry) -> Self {
        FeePriority {
            fee: entry.meta.fee(),
            num_ops: entry.meta.num_ops(),
            hash: *entry.meta.hash(),
        }
    }
}

/// Transaction mempool.
pub struct Mempool {
    /// Transactions by hash (for dedup and lookup)
    by_hash: HashMap<TxHash, MempoolEntry>,

    /// Fee-sorted index (for nomination)
    by_fee: BTreeSet<FeePriority>,

    /// Maximum number of transactions to hold
    max_size: usize,

    /// Maximum age before eviction
    max_age: Duration,
}

impl Mempool {
    /// Create a new mempool with given limits.
    pub fn new(max_size: usize, max_age: Duration) -> Self {
        Self {
            by_hash: HashMap::with_capacity(max_size),
            by_fee: BTreeSet::new(),
            max_size,
            max_age,
        }
    }

    /// Add a transaction to the mempool.
    ///
    /// Returns true if the transaction was added (not a duplicate).
    pub fn insert(&mut self, meta: Arc<ValidatedTx>) -> bool {
        let hash = *meta.hash();

        // Check for duplicate
        if self.by_hash.contains_key(&hash) {
            trace!("Duplicate transaction: {:?}", &hash[..4]);
            return false;
        }

        // Evict if at capacity
        while self.by_hash.len() >= self.max_size {
            self.evict_lowest_fee();
        }

        let entry = MempoolEntry {
            meta,
            received_at: Instant::now(),
        };
        self.by_fee.insert(FeePriority::of(&entry));
        self.by_hash.insert(hash, entry);
        true
    }

    /// Check if a transaction is in the mempool.
    pub fn contains(&self, hash: &TxHash) -> bool {
        self.by_hash.contains_key(hash)
    }

    /// Get a transaction by hash.
    pub fn get(&self, hash: &TxHash) -> Option<&Arc<ValidatedTx>> {
        self.by_hash.get(hash).map(|entry| &entry.meta)
    }

    /// Remove a transaction by hash, returning the removed tx if present.
    pub fn remove(&mut self, hash: &TxHash) -> Option<Arc<ValidatedTx>> {
        let entry = self.by_hash.remove(hash)?;
        self.by_fee.remove(&FeePriority::of(&entry));
        Some(entry.meta)
    }

    /// Get the top N transactions by fee (for nomination).
    pub fn top_by_fee(&self, n: usize) -> Vec<TxHash> {
        self.by_fee.iter().take(n).map(|p| p.hash).collect()
    }

    /// Remove transactions that are too old.
    pub fn evict_expired(&mut self) -> usize {
        let now = Instant::now();
        let to_remove: Vec<TxHash> = self
            .by_hash
            .values()
            .filter(|entry| now.duration_since(entry.received_at) > self.max_age)
            .map(|entry| *entry.meta.hash())
            .collect();

        let count = to_remove.len();
        for hash in to_remove {
            self.remove(&hash);
        }
        count
    }

    /// Current number of transactions.
    pub fn len(&self) -> usize {
        self.by_hash.len()
    }

    /// Is the mempool empty?
    pub fn is_empty(&self) -> bool {
        self.by_hash.is_empty()
    }

    /// Evict the lowest-fee transaction.
    fn evict_lowest_fee(&mut self) {
        if let Some(priority) = self.by_fee.iter().last().cloned() {
            trace!("Evicting lowest-fee tx: {:?}", &priority.hash[..4]);
            self.remove(&priority.hash);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xdr::tests::valid_transaction_xdr;

    /// Build a validated tx with a distinct hash per `seq`.
    fn make_tx(fee: u64, num_ops: u32, seq: i64) -> Arc<ValidatedTx> {
        let bytes = valid_transaction_xdr(fee as u32, seq, num_ops as usize);
        ValidatedTx::from_core_trusted(bytes, fee, num_ops).unwrap()
    }

    #[test]
    fn test_insert_and_get() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx = make_tx(1000, 1, 1);
        let hash = *tx.hash();

        assert!(mempool.insert(tx));
        assert!(mempool.contains(&hash));
        assert_eq!(mempool.len(), 1);
        assert_eq!(mempool.get(&hash).unwrap().fee(), 1000);
    }

    #[test]
    fn test_dedup() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx = make_tx(1000, 1, 1);

        assert!(mempool.insert(tx.clone()));
        assert!(!mempool.insert(tx)); // duplicate
        assert_eq!(mempool.len(), 1);
    }

    #[test]
    fn test_fee_ordering() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let low = make_tx(100, 1, 1);
        let mid = make_tx(500, 1, 2);
        let high = make_tx(1000, 1, 3);
        let (low_h, mid_h, high_h) = (*low.hash(), *mid.hash(), *high.hash());

        mempool.insert(low);
        mempool.insert(high);
        mempool.insert(mid);

        let top = mempool.top_by_fee(3);
        assert_eq!(top, vec![high_h, mid_h, low_h]);
    }

    #[test]
    fn test_fee_per_op_ordering() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx1 = make_tx(200, 2, 1); // 100 per op
        let tx2 = make_tx(150, 1, 2); // 150 per op (higher priority)
        let (h1, h2) = (*tx1.hash(), *tx2.hash());

        mempool.insert(tx1);
        mempool.insert(tx2);

        let top = mempool.top_by_fee(2);
        assert_eq!(top, vec![h2, h1]);
    }

    #[test]
    fn test_evict_at_capacity_removes_lowest_fee() {
        let mut mempool = Mempool::new(3, Duration::from_secs(300));
        let tx1 = make_tx(100, 1, 1); // lowest fee
        let hash1 = *tx1.hash();
        mempool.insert(tx1);
        mempool.insert(make_tx(200, 1, 2));
        mempool.insert(make_tx(300, 1, 3));
        assert_eq!(mempool.len(), 3);

        let tx4 = make_tx(400, 1, 4);
        let hash4 = *tx4.hash();
        mempool.insert(tx4);

        assert_eq!(mempool.len(), 3);
        assert!(!mempool.contains(&hash1)); // evicted
        assert!(mempool.contains(&hash4)); // kept
    }

    #[test]
    fn test_remove() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx = make_tx(1000, 1, 1);
        let hash = *tx.hash();

        mempool.insert(tx);
        assert!(mempool.remove(&hash).is_some());
        assert!(!mempool.contains(&hash));
        assert_eq!(mempool.len(), 0);
    }

    #[test]
    fn test_remove_nonexistent() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        assert!(mempool.remove(&[0u8; 32]).is_none());
    }

    #[test]
    fn test_stress_insert_many() {
        let mut mempool = Mempool::new(1000, Duration::from_secs(300));
        for i in 0..200i64 {
            assert!(mempool.insert(make_tx((i as u64 + 1) * 10, 1, i)));
        }
        assert_eq!(mempool.len(), 200);
        assert_eq!(mempool.top_by_fee(10).len(), 10);
    }

    #[test]
    fn test_top_by_fee_empty() {
        let mempool = Mempool::new(100, Duration::from_secs(300));
        assert!(mempool.top_by_fee(10).is_empty());
    }

    #[test]
    fn test_top_by_fee_fewer_than_requested() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        mempool.insert(make_tx(100, 1, 1));
        mempool.insert(make_tx(200, 1, 2));
        assert_eq!(mempool.top_by_fee(10).len(), 2);
    }

    #[test]
    fn test_remove_all() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let mut hashes = Vec::new();
        for i in 0..10i64 {
            let tx = make_tx(100, 1, i);
            hashes.push(*tx.hash());
            mempool.insert(tx);
        }
        assert_eq!(mempool.len(), 10);
        for hash in hashes {
            mempool.remove(&hash);
        }
        assert_eq!(mempool.len(), 0);
        assert!(mempool.top_by_fee(10).is_empty());
    }

    #[test]
    fn test_zero_fee_tx_sorts_last() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        mempool.insert(make_tx(0, 1, 1));
        let high = make_tx(1000, 1, 2);
        let high_hash = *high.hash();
        mempool.insert(high);

        let top = mempool.top_by_fee(2);
        assert_eq!(top.len(), 2);
        assert_eq!(top[0], high_hash);
        assert_eq!(mempool.get(&top[0]).unwrap().fee(), 1000);
        assert_eq!(mempool.get(&top[1]).unwrap().fee(), 0);
    }

    #[test]
    fn test_evict_expired() {
        let mut mempool = Mempool::new(100, Duration::from_millis(0));
        mempool.insert(make_tx(100, 1, 1));
        // With a zero max_age every entry is immediately expired.
        std::thread::sleep(Duration::from_millis(1));
        assert_eq!(mempool.evict_expired(), 1);
        assert!(mempool.is_empty());
    }
}
