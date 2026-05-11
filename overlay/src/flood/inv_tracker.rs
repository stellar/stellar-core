//! INV Tracker - tracks which peers have advertised which transactions.
//!
//! Used for:
//! 1. Round-robin GETDATA requests across peers who advertised a TX
//! 2. Avoiding sending INV back to peers who already know about a TX

use libp2p::PeerId;
use lru::LruCache;
use std::num::NonZeroUsize;

/// Default capacity for tracking TX sources
pub const INV_TRACKER_CAPACITY: usize = 100_000;

/// Tracks which peers have advertised which transactions
#[derive(Debug)]
pub struct InvTracker {
    /// TX hash -> ordered list of peers who INV'd it
    sources: LruCache<[u8; 32], Vec<PeerId>>,
    /// TX hash -> next peer index for round-robin GETDATA
    next_idx: LruCache<[u8; 32], usize>,
}

impl InvTracker {
    pub fn new() -> Self {
        Self::with_capacity(INV_TRACKER_CAPACITY)
    }

    fn with_capacity(capacity: usize) -> Self {
        InvTracker {
            sources: LruCache::new(NonZeroUsize::new(capacity).unwrap()),
            next_idx: LruCache::new(NonZeroUsize::new(capacity).unwrap()),
        }
    }

    /// Record that a peer has advertised a TX. Returns true if this is the first INV for this TX.
    pub fn record_source(&mut self, hash: [u8; 32], peer: PeerId) -> bool {
        let is_first = !self.sources.contains(&hash);

        let sources = self.sources.get_or_insert_mut(hash, Vec::new);
        // Avoid duplicates
        if !sources.contains(&peer) {
            sources.push(peer);
        }

        is_first
    }

    /// Get the next peer to request from (round-robin)
    pub fn get_next_peer(&mut self, hash: &[u8; 32]) -> Option<PeerId> {
        let sources = self.sources.get(hash)?;
        if sources.is_empty() {
            return None;
        }

        let idx = *self.next_idx.get_or_insert(*hash, || 0);
        let peer = sources[idx % sources.len()];

        // Advance for next call
        self.next_idx.put(*hash, idx + 1);

        Some(peer)
    }

    /// Peek at sources without updating LRU (for read-only access)
    pub fn peek_sources(&self, hash: &[u8; 32]) -> Option<&Vec<PeerId>> {
        self.sources.peek(hash)
    }
}

impl Default for InvTracker {
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
    fn test_inv_tracker_single_source() {
        let mut tracker = InvTracker::new();
        let peer = PeerId::random();

        let is_first = tracker.record_source(hash(1), peer);
        assert!(is_first);

        let sources = tracker.peek_sources(&hash(1)).unwrap();
        assert_eq!(sources.len(), 1);
        assert_eq!(sources[0], peer);
    }

    #[test]
    fn test_inv_tracker_multiple_sources() {
        let mut tracker = InvTracker::new();
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();
        let peer3 = PeerId::random();

        assert!(tracker.record_source(hash(1), peer1));
        assert!(!tracker.record_source(hash(1), peer2)); // Not first
        assert!(!tracker.record_source(hash(1), peer3));

        let sources = tracker.peek_sources(&hash(1)).unwrap();
        assert_eq!(sources.len(), 3);
        // Order preserved
        assert_eq!(sources[0], peer1);
        assert_eq!(sources[1], peer2);
        assert_eq!(sources[2], peer3);
    }

    #[test]
    fn test_inv_tracker_dedup_sources() {
        let mut tracker = InvTracker::new();
        let peer = PeerId::random();

        tracker.record_source(hash(1), peer);
        tracker.record_source(hash(1), peer); // Same peer again

        let sources = tracker.peek_sources(&hash(1)).unwrap();
        assert_eq!(sources.len(), 1); // Not duplicated
    }

    #[test]
    fn test_inv_tracker_round_robin() {
        let mut tracker = InvTracker::new();
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();
        let peer3 = PeerId::random();

        tracker.record_source(hash(1), peer1);
        tracker.record_source(hash(1), peer2);
        tracker.record_source(hash(1), peer3);

        // Round-robin through peers
        assert_eq!(tracker.get_next_peer(&hash(1)), Some(peer1));
        assert_eq!(tracker.get_next_peer(&hash(1)), Some(peer2));
        assert_eq!(tracker.get_next_peer(&hash(1)), Some(peer3));
        assert_eq!(tracker.get_next_peer(&hash(1)), Some(peer1)); // Wraps around
    }
}
