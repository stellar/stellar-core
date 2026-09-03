//! Transaction mempool.
//!
//! Stores transactions waiting to be included in the ledger, indexed for:
//! - Deduplication by hash
//! - Fee-based ordering for nomination, kept separately per transaction kind
//!   (classic / Soroban) because Core fills the two tx set phases
//!   independently, with independent count and byte limits.

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tracing::trace;

use crate::wire::{TxKind, ValidatedTx};

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
    fee: i64,
    /// Number of ops (lower is better for same fee)
    num_ops: u32,
    /// Hash for tie-breaking
    hash: TxHash,
}

impl Ord for FeePriority {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        // Higher fee per op = higher priority
        // fee1/ops1 > fee2/ops2 iff fee1*ops2 > fee2*ops1
        let left = self.fee * (other.num_ops as i64);
        let right = other.fee * (self.num_ops as i64);

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

/// How much of one transaction kind Core wants from the mempool: at most
/// `max_count` transactions totalling at most `max_bytes` of envelope XDR.
/// Both are upper bounds on what fits in the corresponding tx set phase.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TxBudget {
    pub max_count: usize,
    pub max_bytes: usize,
}

/// Transaction mempool.
pub struct Mempool {
    /// Transactions by hash (for dedup and lookup)
    by_hash: HashMap<TxHash, MempoolEntry>,

    /// Fee-sorted index per transaction kind (indexed by `TxKind::index`),
    /// used to answer nomination pulls for each tx set phase.
    by_fee: [BTreeSet<FeePriority>; 2],

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
            by_fee: [BTreeSet::new(), BTreeSet::new()],
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
        self.by_fee[entry.meta.kind().index()].insert(FeePriority::of(&entry));
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
        self.by_fee[entry.meta.kind().index()].remove(&FeePriority::of(&entry));
        Some(entry.meta)
    }

    /// Get the highest-priority transactions of one kind that fit in
    /// `budget` (for nomination).
    ///
    /// Walks the fee-sorted index in priority order, stopping at
    /// `budget.max_count` transactions. A transaction whose bytes would push
    /// the total over `budget.max_bytes` is skipped and the walk continues
    /// with smaller ones, mirroring how Core's surge pricing fills a phase.
    pub fn top_by_fee(&self, kind: TxKind, budget: TxBudget) -> Vec<Arc<ValidatedTx>> {
        let mut out = Vec::with_capacity(budget.max_count.min(self.by_hash.len()));
        let mut remaining_bytes = budget.max_bytes;
        if budget.max_count == 0 || remaining_bytes == 0 {
            return out;
        }
        for priority in &self.by_fee[kind.index()] {
            let Some(entry) = self.by_hash.get(&priority.hash) else {
                continue;
            };
            let size = entry.meta.bytes().len();
            if size > remaining_bytes {
                continue;
            }
            out.push(Arc::clone(&entry.meta));
            remaining_bytes -= size;
            if out.len() >= budget.max_count || remaining_bytes == 0 {
                break;
            }
        }
        out
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

    /// Number of transactions of one kind.
    pub fn len_of_kind(&self, kind: TxKind) -> usize {
        self.by_fee[kind.index()].len()
    }

    /// Is the mempool empty?
    pub fn is_empty(&self) -> bool {
        self.by_hash.is_empty()
    }

    /// Evict the lowest-priority transaction across both kinds.
    fn evict_lowest_fee(&mut self) {
        // Each index is ordered highest-priority-first, so its last element is
        // its lowest-priority tx; the overall victim is the lower of the two
        // tails (which compares *greater* under `FeePriority`'s ordering).
        let victim = TxKind::ALL
            .iter()
            .filter_map(|kind| self.by_fee[kind.index()].iter().next_back().copied())
            .max();
        if let Some(priority) = victim {
            trace!("Evicting lowest-fee tx: {:?}", &priority.hash[..4]);
            self.remove(&priority.hash);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xdr::tests::valid_transaction_xdr;

    /// Build a validated classic tx with a distinct hash per `seq`.
    fn make_tx(fee: i64, num_ops: u32, seq: i64) -> Arc<ValidatedTx> {
        make_tx_of_kind(fee, num_ops, seq, TxKind::Classic)
    }

    fn make_tx_of_kind(fee: i64, num_ops: u32, seq: i64, kind: TxKind) -> Arc<ValidatedTx> {
        let bytes = valid_transaction_xdr(fee as u32, seq, num_ops as usize);
        ValidatedTx::from_core_trusted(bytes, fee, num_ops, kind).unwrap()
    }

    /// Budget that never binds, for tests about ordering only.
    fn unbounded(max_count: usize) -> TxBudget {
        TxBudget {
            max_count,
            max_bytes: usize::MAX,
        }
    }

    fn top_hashes(mempool: &Mempool, kind: TxKind, n: usize) -> Vec<TxHash> {
        mempool
            .top_by_fee(kind, unbounded(n))
            .iter()
            .map(|tx| *tx.hash())
            .collect()
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

        assert_eq!(
            top_hashes(&mempool, TxKind::Classic, 3),
            vec![high_h, mid_h, low_h]
        );
    }

    #[test]
    fn test_fee_per_op_ordering() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        let tx1 = make_tx(200, 2, 1); // 100 per op
        let tx2 = make_tx(150, 1, 2); // 150 per op (higher priority)
        let (h1, h2) = (*tx1.hash(), *tx2.hash());

        mempool.insert(tx1);
        mempool.insert(tx2);

        assert_eq!(top_hashes(&mempool, TxKind::Classic, 2), vec![h2, h1]);
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
    fn test_evict_at_capacity_picks_lowest_across_kinds() {
        let mut mempool = Mempool::new(3, Duration::from_secs(300));
        let cheap_soroban = make_tx_of_kind(50, 1, 1, TxKind::Soroban);
        let cheap_hash = *cheap_soroban.hash();
        mempool.insert(cheap_soroban);
        mempool.insert(make_tx(200, 1, 2));
        mempool.insert(make_tx(300, 1, 3));

        // Capacity reached: the Soroban tx is the lowest-priority entry
        // overall, so it must be the victim even though the newcomer is
        // classic.
        mempool.insert(make_tx(400, 1, 4));
        assert_eq!(mempool.len(), 3);
        assert!(!mempool.contains(&cheap_hash));
        assert_eq!(mempool.len_of_kind(TxKind::Soroban), 0);
        assert_eq!(mempool.len_of_kind(TxKind::Classic), 3);
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
        assert_eq!(mempool.len_of_kind(TxKind::Classic), 0);
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
            assert!(mempool.insert(make_tx((i + 1) * 10, 1, i)));
        }
        assert_eq!(mempool.len(), 200);
        assert_eq!(top_hashes(&mempool, TxKind::Classic, 10).len(), 10);
    }

    #[test]
    fn test_top_by_fee_empty() {
        let mempool = Mempool::new(100, Duration::from_secs(300));
        assert!(top_hashes(&mempool, TxKind::Classic, 10).is_empty());
        assert!(top_hashes(&mempool, TxKind::Soroban, 10).is_empty());
    }

    #[test]
    fn test_top_by_fee_fewer_than_requested() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        mempool.insert(make_tx(100, 1, 1));
        mempool.insert(make_tx(200, 1, 2));
        assert_eq!(top_hashes(&mempool, TxKind::Classic, 10).len(), 2);
    }

    #[test]
    fn test_top_by_fee_is_per_kind() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        // A Soroban tx with a much higher fee must not crowd classic txs out
        // of the classic pull, and vice versa.
        let soroban = make_tx_of_kind(1_000_000, 1, 1, TxKind::Soroban);
        let classic_a = make_tx(100, 1, 2);
        let classic_b = make_tx(200, 1, 3);
        let (s_h, a_h, b_h) = (*soroban.hash(), *classic_a.hash(), *classic_b.hash());
        mempool.insert(soroban);
        mempool.insert(classic_a);
        mempool.insert(classic_b);

        assert_eq!(top_hashes(&mempool, TxKind::Classic, 10), vec![b_h, a_h]);
        assert_eq!(top_hashes(&mempool, TxKind::Soroban, 10), vec![s_h]);
        assert_eq!(mempool.len_of_kind(TxKind::Classic), 2);
        assert_eq!(mempool.len_of_kind(TxKind::Soroban), 1);
    }

    #[test]
    fn test_top_by_fee_respects_count_budget() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        for i in 0..10i64 {
            mempool.insert(make_tx(100 + i, 1, i));
        }
        assert_eq!(top_hashes(&mempool, TxKind::Classic, 3).len(), 3);
        assert!(top_hashes(&mempool, TxKind::Classic, 0).is_empty());
    }

    #[test]
    fn test_top_by_fee_respects_byte_budget() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        // Three one-op txs of identical size but different fees, plus one
        // big (many-op) tx with the highest fee-per-op.
        let small_size = make_tx(100, 1, 1).bytes().len();
        let big = make_tx(100_000, 20, 4);
        let big_size = big.bytes().len();
        assert!(big_size > small_size);
        let big_hash = *big.hash();
        let mid = make_tx(300, 1, 2);
        let mid_hash = *mid.hash();
        let top = make_tx(500, 1, 3);
        let top_hash = *top.hash();
        mempool.insert(make_tx(100, 1, 1));
        mempool.insert(mid);
        mempool.insert(top);
        mempool.insert(big);

        // Budget for exactly two small txs: the big tx sorts first but does
        // not fit, so it is skipped and the two best-fitting txs are taken.
        let got = mempool.top_by_fee(
            TxKind::Classic,
            TxBudget {
                max_count: 10,
                max_bytes: 2 * small_size,
            },
        );
        let hashes: Vec<TxHash> = got.iter().map(|tx| *tx.hash()).collect();
        assert_eq!(hashes, vec![top_hash, mid_hash]);
        assert!(got.iter().map(|tx| tx.bytes().len()).sum::<usize>() <= 2 * small_size);

        // A budget large enough for the big tx takes it first.
        let got = mempool.top_by_fee(
            TxKind::Classic,
            TxBudget {
                max_count: 1,
                max_bytes: big_size,
            },
        );
        assert_eq!(got.len(), 1);
        assert_eq!(*got[0].hash(), big_hash);

        // A zero byte budget yields nothing.
        assert!(mempool
            .top_by_fee(
                TxKind::Classic,
                TxBudget {
                    max_count: 10,
                    max_bytes: 0,
                },
            )
            .is_empty());
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
        assert!(top_hashes(&mempool, TxKind::Classic, 10).is_empty());
    }

    #[test]
    fn test_zero_fee_tx_sorts_last() {
        let mut mempool = Mempool::new(100, Duration::from_secs(300));
        mempool.insert(make_tx(0, 1, 1));
        let high = make_tx(1000, 1, 2);
        let high_hash = *high.hash();
        mempool.insert(high);

        let top = top_hashes(&mempool, TxKind::Classic, 2);
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
        assert_eq!(mempool.len_of_kind(TxKind::Classic), 0);
    }
}
