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
}

impl Default for InvBatcher {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_peer(_n: u8) -> PeerId {
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
    fn test_inv_batcher_flush_empty() {
        let mut batcher = InvBatcher::new();
        let peer = make_peer(1);

        // Flush without adding anything
        assert!(batcher.flush(&peer).is_none());
    }
}
