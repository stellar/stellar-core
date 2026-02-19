//! Transaction mempool.
//!
//! Stores transactions waiting to be included in the ledger, indexed for:
//! - Deduplication by hash
//! - Fee-based ordering for nomination
//! - Sequence tracking by account

use blake2::{Blake2b, Digest};
use digest::consts::U32;
use std::collections::{BTreeSet, HashMap};
use std::time::{Duration, Instant};
use tracing::trace;

/// 32-byte transaction hash
pub type TxHash = [u8; 32];

/// Account ID (32-byte public key)
pub type AccountId = [u8; 32];

/// A transaction with its metadata
#[derive(Debug, Clone)]
pub struct TxEntry {
    /// Full transaction bytes
    pub data: Vec<u8>,
    /// Transaction hash (derived from data)
    pub hash: TxHash,
    /// Source account
    pub source_account: AccountId,
    /// Sequence number
    pub sequence: u64,
    /// Fee (in stroops)
    pub fee: u64,
    /// Number of operations
    pub num_ops: u32,
    /// When we received this transaction
    pub received_at: Instant,
    /// Which peer sent it (0 = local submission)
    pub from_peer: u64,
}

impl TxEntry {
    /// Compute fee per operation (for priority ordering)
    /// Uses cross-multiplication to avoid division: f1/n1 > f2/n2 iff f1*n2 > f2*n1
    pub fn fee_priority(&self) -> (u64, u64) {
        (self.fee, self.num_ops as u64)
    }
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

/// Transaction mempool.
pub struct Mempool {
    /// Transactions by hash (for dedup and lookup)
    by_hash: HashMap<TxHash, TxEntry>,

    /// Fee-sorted index (for nomination)
    by_fee: BTreeSet<FeePriority>,

    /// Transactions by account (for sequence tracking)
    by_account: HashMap<AccountId, Vec<TxHash>>,

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
            by_account: HashMap::new(),
            max_size,
            max_age,
        }
    }

    /// Add a transaction to the mempool.
    ///
    /// Returns true if the transaction was added (not a duplicate).
    pub fn insert(&mut self, tx: TxEntry) -> bool {
        // Check for duplicate
        if self.by_hash.contains_key(&tx.hash) {
            trace!("Duplicate transaction: {:?}", &tx.hash[..4]);
            return false;
        }

        // Evict if at capacity
        while self.by_hash.len() >= self.max_size {
            self.evict_lowest_fee();
        }

        // Add to fee index
        let priority = FeePriority {
            fee: tx.fee,
            num_ops: tx.num_ops,
            hash: tx.hash,
        };
        self.by_fee.insert(priority);

        // Add to account index
        self.by_account
            .entry(tx.source_account)
            .or_default()
            .push(tx.hash);

        // Add to hash map
        self.by_hash.insert(tx.hash, tx);

        true
    }

    /// Check if a transaction is in the mempool.
    pub fn contains(&self, hash: &TxHash) -> bool {
        self.by_hash.contains_key(hash)
    }

    /// Get a transaction by hash.
    pub fn get(&self, hash: &TxHash) -> Option<&TxEntry> {
        self.by_hash.get(hash)
    }

    /// Remove a transaction by hash.
    pub fn remove(&mut self, hash: &TxHash) -> Option<TxEntry> {
        if let Some(tx) = self.by_hash.remove(hash) {
            // Remove from fee index
            let priority = FeePriority {
                fee: tx.fee,
                num_ops: tx.num_ops,
                hash: tx.hash,
            };
            self.by_fee.remove(&priority);

            // Remove from account index
            if let Some(account_txs) = self.by_account.get_mut(&tx.source_account) {
                account_txs.retain(|h| h != hash);
                if account_txs.is_empty() {
                    self.by_account.remove(&tx.source_account);
                }
            }

            Some(tx)
        } else {
            None
        }
    }

    /// Get the top N transactions by fee (for nomination).
    pub fn top_by_fee(&self, n: usize) -> Vec<TxHash> {
        self.by_fee.iter().take(n).map(|p| p.hash).collect()
    }

    /// Get all transactions from an account, sorted by sequence.
    pub fn by_account(&self, account: &AccountId) -> Vec<&TxEntry> {
        let mut txs: Vec<&TxEntry> = self
            .by_account
            .get(account)
            .map(|hashes| hashes.iter().filter_map(|h| self.by_hash.get(h)).collect())
            .unwrap_or_default();

        txs.sort_by_key(|tx| tx.sequence);
        txs
    }

    /// Remove transactions that are too old.
    pub fn evict_expired(&mut self) {
        let now = Instant::now();
        let to_remove: Vec<TxHash> = self
            .by_hash
            .values()
            .filter(|tx| now.duration_since(tx.received_at) > self.max_age)
            .map(|tx| tx.hash)
            .collect();

        for hash in to_remove {
            self.remove(&hash);
        }
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

/// Compute transaction hash from raw bytes.
pub fn compute_tx_hash(data: &[u8]) -> TxHash {
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    hasher.update(data);
    let result = hasher.finalize();
    let mut hash = [0u8; 32];
    hash.copy_from_slice(&result);
    hash
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_tx(fee: u64, num_ops: u32, seq: u64, account_byte: u8) -> TxEntry {
        let data = vec![account_byte, (seq & 0xFF) as u8, (fee & 0xFF) as u8];
        let hash = compute_tx_hash(&data);
        let mut source_account = [0u8; 32];
        source_account[0] = account_byte;

        TxEntry {
            data,
            hash,
            source_account,
            sequence: seq,
            fee,
            num_ops,
            received_at: Instant::now(),
            from_peer: 0,
        }
    }

    #[test]
    fn test_insert_and_get() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx = make_tx(1000, 1, 1, 1);
        let hash = tx.hash;

        assert!(mempool.insert(tx));
        assert!(mempool.contains(&hash));
        assert_eq!(mempool.len(), 1);

        let retrieved = mempool.get(&hash).unwrap();
        assert_eq!(retrieved.fee, 1000);
    }

    #[test]
    fn test_dedup() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx = make_tx(1000, 1, 1, 1);
        let _hash = tx.hash;

        assert!(mempool.insert(tx.clone()));
        assert!(!mempool.insert(tx)); // duplicate

        assert_eq!(mempool.len(), 1);
    }

    #[test]
    fn test_fee_ordering() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // Insert transactions with different fees
        let low_fee = make_tx(100, 1, 1, 1);
        let mid_fee = make_tx(500, 1, 2, 2);
        let high_fee = make_tx(1000, 1, 3, 3);

        let low_hash = low_fee.hash;
        let mid_hash = mid_fee.hash;
        let high_hash = high_fee.hash;

        mempool.insert(low_fee);
        mempool.insert(high_fee);
        mempool.insert(mid_fee);

        // Top by fee should return highest first
        let top = mempool.top_by_fee(3);
        assert_eq!(top[0], high_hash);
        assert_eq!(top[1], mid_hash);
        assert_eq!(top[2], low_hash);
    }

    #[test]
    fn test_fee_per_op_ordering() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // 200 fee / 2 ops = 100 per op
        let tx1 = make_tx(200, 2, 1, 1);
        // 150 fee / 1 op = 150 per op (higher priority)
        let tx2 = make_tx(150, 1, 2, 2);

        let hash1 = tx1.hash;
        let hash2 = tx2.hash;

        mempool.insert(tx1);
        mempool.insert(tx2);

        // tx2 has higher fee-per-op, should be first
        let top = mempool.top_by_fee(2);
        assert_eq!(top[0], hash2); // 150/1 = 150 per op
        assert_eq!(top[1], hash1); // 200/2 = 100 per op
    }

    #[test]
    fn test_evict_at_capacity() {
        let mut mempool = Mempool::new(3, Duration::from_secs(300));

        let tx1 = make_tx(100, 1, 1, 1); // lowest fee
        let tx2 = make_tx(200, 1, 2, 2);
        let tx3 = make_tx(300, 1, 3, 3);
        let tx4 = make_tx(400, 1, 4, 4); // highest fee

        let hash1 = tx1.hash;
        let hash4 = tx4.hash;

        mempool.insert(tx1);
        mempool.insert(tx2);
        mempool.insert(tx3);

        assert_eq!(mempool.len(), 3);

        // Insert tx4 should evict tx1 (lowest fee)
        mempool.insert(tx4);

        assert_eq!(mempool.len(), 3);
        assert!(!mempool.contains(&hash1)); // evicted
        assert!(mempool.contains(&hash4)); // kept
    }

    #[test]
    fn test_remove() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx = make_tx(1000, 1, 1, 1);
        let hash = tx.hash;

        mempool.insert(tx);
        assert!(mempool.contains(&hash));

        let removed = mempool.remove(&hash);
        assert!(removed.is_some());
        assert!(!mempool.contains(&hash));
        assert_eq!(mempool.len(), 0);
    }

    #[test]
    fn test_by_account() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // Same account, different sequences
        let tx1 = make_tx(100, 1, 1, 42);
        let tx2 = make_tx(200, 1, 2, 42);
        let tx3 = make_tx(300, 1, 3, 42);

        mempool.insert(tx2.clone()); // insert out of order
        mempool.insert(tx1.clone());
        mempool.insert(tx3.clone());

        let account = tx1.source_account;
        let account_txs = mempool.by_account(&account);

        assert_eq!(account_txs.len(), 3);
        // Should be sorted by sequence
        assert_eq!(account_txs[0].sequence, 1);
        assert_eq!(account_txs[1].sequence, 2);
        assert_eq!(account_txs[2].sequence, 3);
    }

    #[test]
    fn test_compute_tx_hash() {
        let data1 = b"transaction data 1";
        let data2 = b"transaction data 2";

        let hash1 = compute_tx_hash(data1);
        let hash2 = compute_tx_hash(data2);

        // Different data should produce different hashes
        assert_ne!(hash1, hash2);

        // Same data should produce same hash
        assert_eq!(hash1, compute_tx_hash(data1));
    }

    // ═══ Additional Tests ═══

    #[test]
    fn test_stress_insert_many() {
        let mut mempool = Mempool::new(1000, Duration::from_secs(300));

        // Insert 200 transactions (staying within u8 range for account)
        for i in 0..200u8 {
            let tx = make_tx((i as u64 + 1) * 10, 1, i as u64, i);
            assert!(mempool.insert(tx));
        }

        assert_eq!(mempool.len(), 200);

        // Verify ordering - top should be highest fee
        let top = mempool.top_by_fee(10);
        assert_eq!(top.len(), 10);
    }

    #[test]
    fn test_insert_at_capacity_evicts_lowest_fee() {
        let mut mempool = Mempool::new(3, Duration::from_secs(300));

        // Fill with TXs
        let tx1 = make_tx(500, 1, 1, 1); // Will be evicted when 4th TX added
        let tx2 = make_tx(600, 1, 2, 2);
        let tx3 = make_tx(700, 1, 3, 3);

        let hash1 = tx1.hash;
        mempool.insert(tx1);
        mempool.insert(tx2);
        mempool.insert(tx3);

        assert_eq!(mempool.len(), 3);
        assert!(mempool.contains(&hash1));

        // Insert higher fee TX - lowest fee (tx1) should be evicted
        let high_fee = make_tx(800, 1, 4, 4);
        let high_hash = high_fee.hash;
        mempool.insert(high_fee);

        // tx1 (lowest fee) should be evicted, high_fee should be present
        assert!(!mempool.contains(&hash1));
        assert!(mempool.contains(&high_hash));
        assert_eq!(mempool.len(), 3);
    }

    #[test]
    fn test_top_by_fee_empty() {
        let mempool = Mempool::new(100, Duration::from_secs(300));
        let top = mempool.top_by_fee(10);
        assert!(top.is_empty());
    }

    #[test]
    fn test_top_by_fee_fewer_than_requested() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        mempool.insert(make_tx(100, 1, 1, 1));
        mempool.insert(make_tx(200, 1, 2, 2));

        let top = mempool.top_by_fee(10);
        assert_eq!(top.len(), 2);
    }

    #[test]
    fn test_remove_nonexistent() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        let result = mempool.remove(&[0u8; 32]);
        assert!(result.is_none());
    }

    #[test]
    fn test_by_account_empty() {
        let mempool = Mempool::new(100, Duration::from_secs(300));

        let result = mempool.by_account(&[0u8; 32]);
        assert!(result.is_empty());
    }

    #[test]
    fn test_by_account_nonexistent() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // Insert TX for one account
        mempool.insert(make_tx(100, 1, 1, 1));

        // Query different account
        let result = mempool.by_account(&[99u8; 32]);
        assert!(result.is_empty());
    }

    #[test]
    fn test_remove_all() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // Insert 10 TXs
        let mut hashes = Vec::new();
        for i in 0..10u8 {
            let tx = make_tx(100, 1, i as u64, i);
            hashes.push(tx.hash);
            mempool.insert(tx);
        }

        assert_eq!(mempool.len(), 10);

        // Remove all
        for hash in hashes {
            mempool.remove(&hash);
        }

        assert_eq!(mempool.len(), 0);
        assert!(mempool.top_by_fee(10).is_empty());
    }

    #[test]
    fn test_zero_fee_tx() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // TX with zero fee should be accepted (validation happens elsewhere)
        let tx = make_tx(0, 1, 1, 1);
        assert!(mempool.insert(tx));
        assert_eq!(mempool.len(), 1);

        // Zero fee TX should sort to end (lowest priority)
        let tx_high_fee = make_tx(1000, 1, 2, 2);
        let high_fee_hash = tx_high_fee.hash;
        mempool.insert(tx_high_fee);

        let top = mempool.top_by_fee(2);
        assert_eq!(top.len(), 2);
        // High fee should be first
        assert_eq!(top[0], high_fee_hash, "High fee TX should be first");
        // Verify by looking up the actual fee
        assert_eq!(mempool.get(&top[0]).unwrap().fee, 1000);
        assert_eq!(mempool.get(&top[1]).unwrap().fee, 0);
    }

    #[test]
    fn test_same_account_different_seq() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));

        // Insert two TXs from same account with different sequences
        let account = [42u8; 32];
        let tx1 = TxEntry {
            data: vec![1],
            hash: [1u8; 32],
            source_account: account,
            sequence: 100,
            fee: 500,
            num_ops: 1,
            received_at: Instant::now(),
            from_peer: 0,
        };
        let tx2 = TxEntry {
            data: vec![2],
            hash: [2u8; 32],
            source_account: account,
            sequence: 101,
            fee: 500,
            num_ops: 1,
            received_at: Instant::now(),
            from_peer: 0,
        };

        assert!(mempool.insert(tx1));
        assert!(mempool.insert(tx2));
        assert_eq!(mempool.len(), 2);

        // Both TXs should be retrievable by account
        let account_txs = mempool.by_account(&account);
        assert_eq!(account_txs.len(), 2);
    }

    #[test]
    fn test_evict_returns_lowest_fee() {
        let mut mempool = Mempool::new(3, Duration::from_secs(300));

        // Insert 3 TXs with different fees
        let low_fee_tx = make_tx(100, 1, 1, 1);
        let low_fee_hash = low_fee_tx.hash;
        mempool.insert(low_fee_tx);
        mempool.insert(make_tx(300, 1, 2, 2)); // highest
        mempool.insert(make_tx(200, 1, 3, 3)); // middle

        assert_eq!(mempool.len(), 3);

        // Insert 4th TX - should evict the lowest fee (100)
        mempool.insert(make_tx(250, 1, 4, 4));
        assert_eq!(mempool.len(), 3);

        // Verify lowest fee TX was evicted
        assert!(
            !mempool.contains(&low_fee_hash),
            "Lowest fee TX should have been evicted"
        );

        // Verify remaining TXs have expected fees
        let top = mempool.top_by_fee(3);
        let fees: Vec<u64> = top.iter().map(|h| mempool.get(h).unwrap().fee).collect();
        assert!(fees.contains(&300), "Highest fee TX should remain");
        assert!(fees.contains(&250), "New TX should be present");
        assert!(fees.contains(&200), "Middle fee TX should remain");
    }
}
