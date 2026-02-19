//! INV Batcher - batches INV announcements before sending.
//!
//! Instead of sending one INV per TX immediately, we batch INVs and send them:
//! - When batch reaches 1000 entries, OR
//! - After 100ms timeout (whichever comes first)
//!
//! This reduces packet overhead and improves efficiency.

use super::inv_messages::{InvBatch, InvEntry, INV_BATCH_MAX_SIZE};
use libp2p::PeerId;
use std::collections::{HashMap, HashSet};
use std::time::{Duration, Instant};

/// Maximum time to wait before flushing a batch
pub const INV_BATCH_MAX_DELAY: Duration = Duration::from_millis(100);

/// Batches INV entries per peer before sending
#[derive(Debug)]
pub struct InvBatcher {
    /// Pending INVs per peer
    pending: HashMap<PeerId, PeerBatch>,
    /// Max entries before auto-flush
    max_batch_size: usize,
    /// Max delay before flush
    max_delay: Duration,
}

#[derive(Debug)]
struct PeerBatch {
    /// Batch of INV entries
    batch: InvBatch,
    /// When first entry was added (for timeout)
    started_at: Option<Instant>,
    /// Hashes already in this batch (dedup)
    seen: HashSet<[u8; 32]>,
}

impl PeerBatch {
    fn new() -> Self {
        PeerBatch {
            batch: InvBatch::new(),
            started_at: None,
            seen: HashSet::new(),
        }
    }

    /// Add entry and return full batch to send if at capacity.
    /// Always accepts the entry.
    fn add(&mut self, entry: InvEntry, max_size: usize) -> Option<InvBatch> {
        // Dedup within batch
        if self.seen.contains(&entry.hash) {
            return None;
        }
        self.seen.insert(entry.hash);

        if self.started_at.is_none() {
            self.started_at = Some(Instant::now());
        }
        self.batch.push(entry);

        // Return batch if full
        if self.batch.entries.len() >= max_size {
            Some(self.take())
        } else {
            None
        }
    }

    fn is_expired(&self, max_delay: Duration) -> bool {
        self.started_at
            .map(|t| t.elapsed() >= max_delay)
            .unwrap_or(false)
    }

    fn is_empty(&self) -> bool {
        self.batch.entries.is_empty()
    }

    fn take(&mut self) -> InvBatch {
        self.started_at = None;
        self.seen.clear();
        std::mem::take(&mut self.batch)
    }
}

impl InvBatcher {
    pub fn new() -> Self {
        InvBatcher {
            pending: HashMap::new(),
            max_batch_size: INV_BATCH_MAX_SIZE,
            max_delay: INV_BATCH_MAX_DELAY,
        }
    }

    /// Create with custom settings (for testing)
    pub fn with_config(max_batch_size: usize, max_delay: Duration) -> Self {
        InvBatcher {
            pending: HashMap::new(),
            max_batch_size,
            max_delay,
        }
    }

    /// Add an INV entry for a peer. Returns batch to send if full.
    pub fn add(&mut self, peer: PeerId, entry: InvEntry) -> Option<InvBatch> {
        let batch = self.pending.entry(peer).or_insert_with(PeerBatch::new);
        batch.add(entry, self.max_batch_size)
    }

    /// Check which peers have expired batches
    pub fn expired_peers(&self) -> Vec<PeerId> {
        self.pending
            .iter()
            .filter(|(_, batch)| batch.is_expired(self.max_delay) && !batch.is_empty())
            .map(|(peer, _)| *peer)
            .collect()
    }

    /// Flush batch for a peer (returns None if empty)
    pub fn flush(&mut self, peer: &PeerId) -> Option<InvBatch> {
        let batch = self.pending.get_mut(peer)?;
        if batch.is_empty() {
            return None;
        }
        Some(batch.take())
    }

    /// Flush all non-empty batches
    pub fn flush_all(&mut self) -> Vec<(PeerId, InvBatch)> {
        let peers: Vec<_> = self.pending.keys().cloned().collect();
        let mut result = Vec::new();
        for peer in peers {
            if let Some(batch) = self.flush(&peer) {
                result.push((peer, batch));
            }
        }
        result
    }

    /// Time until next batch expires (for timer scheduling)
    pub fn time_until_next_expiry(&self) -> Option<Duration> {
        self.pending
            .values()
            .filter_map(|batch| {
                batch.started_at.map(|t| {
                    let elapsed = t.elapsed();
                    if elapsed >= self.max_delay {
                        Duration::ZERO
                    } else {
                        self.max_delay - elapsed
                    }
                })
            })
            .min()
    }

    /// Remove a peer (e.g., on disconnect)
    pub fn remove_peer(&mut self, peer: &PeerId) {
        self.pending.remove(peer);
    }

    /// Number of pending entries across all peers
    pub fn pending_count(&self) -> usize {
        self.pending.values().map(|b| b.batch.entries.len()).sum()
    }
}

impl Default for InvBatcher {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_peer(n: u8) -> PeerId {
        PeerId::random()
    }

    fn make_entry(hash_byte: u8, fee: i64) -> InvEntry {
        InvEntry {
            hash: [hash_byte; 32],
            fee_per_op: fee,
        }
    }

    #[test]
    fn test_inv_batcher_single_entry() {
        let mut batcher = InvBatcher::new();
        let peer = make_peer(1);

        let batch = batcher.add(peer, make_entry(0x01, 100));
        assert!(batch.is_none()); // Not at capacity

        let batch = batcher.flush(&peer).unwrap();
        assert_eq!(batch.entries.len(), 1);
        assert_eq!(batch.entries[0].hash, [0x01; 32]);
    }

    #[test]
    fn test_inv_batcher_batch_by_count() {
        let mut batcher = InvBatcher::with_config(10, Duration::from_secs(60));
        let peer = make_peer(1);

        // Add 9 entries - should not return batch
        for i in 0..9 {
            let batch = batcher.add(peer, make_entry(i, i as i64));
            assert!(batch.is_none());
        }

        // 10th entry should return the full batch
        let batch = batcher.add(peer, make_entry(9, 9));
        assert!(batch.is_some());
        assert_eq!(batch.unwrap().entries.len(), 10);

        // After returning batch, flush should be empty
        assert!(batcher.flush(&peer).is_none());
    }

    #[test]
    fn test_inv_batcher_per_peer_separate() {
        let mut batcher = InvBatcher::with_config(5, Duration::from_secs(60));
        let peer_a = make_peer(1);
        let peer_b = make_peer(2);

        // Add to peer A
        for i in 0..3 {
            batcher.add(peer_a, make_entry(i, i as i64));
        }

        // Add to peer B
        for i in 10..12 {
            batcher.add(peer_b, make_entry(i, i as i64));
        }

        // Flush peer A - should have 3 entries
        let batch_a = batcher.flush(&peer_a).unwrap();
        assert_eq!(batch_a.entries.len(), 3);

        // Flush peer B - should have 2 entries
        let batch_b = batcher.flush(&peer_b).unwrap();
        assert_eq!(batch_b.entries.len(), 2);
    }

    #[test]
    fn test_inv_batcher_dedup_same_peer() {
        let mut batcher = InvBatcher::new();
        let peer = make_peer(1);

        // Add same hash twice
        batcher.add(peer, make_entry(0x42, 100));
        batcher.add(peer, make_entry(0x42, 200)); // Same hash, different fee

        let batch = batcher.flush(&peer).unwrap();
        assert_eq!(batch.entries.len(), 1); // Only one entry
    }

    #[test]
    fn test_inv_batcher_expired_peers() {
        let mut batcher = InvBatcher::with_config(1000, Duration::from_millis(10));
        let peer = make_peer(1);

        batcher.add(peer, make_entry(0x01, 100));

        // Initially not expired
        assert!(batcher.expired_peers().is_empty());

        // Wait for expiry
        std::thread::sleep(Duration::from_millis(15));

        let expired = batcher.expired_peers();
        assert_eq!(expired.len(), 1);
        assert_eq!(expired[0], peer);
    }

    #[test]
    fn test_inv_batcher_flush_all() {
        let mut batcher = InvBatcher::new();
        let peer_a = make_peer(1);
        let peer_b = make_peer(2);

        batcher.add(peer_a, make_entry(0x01, 100));
        batcher.add(peer_b, make_entry(0x02, 200));

        let all = batcher.flush_all();
        assert_eq!(all.len(), 2);
    }

    #[test]
    fn test_inv_batcher_flush_empty() {
        let mut batcher = InvBatcher::new();
        let peer = make_peer(1);

        // Flush without adding anything
        assert!(batcher.flush(&peer).is_none());
    }

    #[test]
    fn test_inv_batcher_add_fills_and_returns() {
        let mut batcher = InvBatcher::with_config(2, Duration::from_secs(60));
        let peer = PeerId::random();

        // Add entry - not full yet
        let batch = batcher.add(peer, make_entry(0x01, 100));
        assert!(batch.is_none());

        // Add another - returns full batch
        let batch = batcher.add(peer, make_entry(0x02, 200));
        assert!(batch.is_some());
        assert_eq!(batch.unwrap().entries.len(), 2);
    }

    #[test]
    fn test_inv_batcher_remove_peer() {
        let mut batcher = InvBatcher::new();
        let peer = make_peer(1);

        batcher.add(peer, make_entry(0x01, 100));
        assert_eq!(batcher.pending_count(), 1);

        batcher.remove_peer(&peer);
        assert_eq!(batcher.pending_count(), 0);
    }

    #[test]
    fn test_inv_batcher_time_until_expiry() {
        let mut batcher = InvBatcher::with_config(1000, Duration::from_millis(100));

        // No pending batches
        assert!(batcher.time_until_next_expiry().is_none());

        let peer = make_peer(1);
        batcher.add(peer, make_entry(0x01, 100));

        // Should have ~100ms until expiry
        let remaining = batcher.time_until_next_expiry().unwrap();
        assert!(remaining <= Duration::from_millis(100));
        assert!(remaining >= Duration::from_millis(90)); // Allow some slack
    }

    #[test]
    fn test_inv_batcher_pending_count() {
        let mut batcher = InvBatcher::new();
        let peer_a = make_peer(1);
        let peer_b = make_peer(2);

        assert_eq!(batcher.pending_count(), 0);

        batcher.add(peer_a, make_entry(0x01, 100));
        batcher.add(peer_a, make_entry(0x02, 200));
        batcher.add(peer_b, make_entry(0x03, 300));

        assert_eq!(batcher.pending_count(), 3);
    }
}
