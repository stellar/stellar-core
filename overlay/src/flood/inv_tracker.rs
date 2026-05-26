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

#[derive(Debug)]
struct InvSource {
    peers: Vec<PeerId>,
    next_idx: usize,
}

impl InvSource {
    fn new() -> Self {
        InvSource {
            peers: Vec::new(),
            next_idx: 0,
        }
    }

    fn next_peer(&mut self) -> Option<PeerId> {
        if self.peers.is_empty() {
            return None;
        }
        let peer = self.peers[self.next_idx % self.peers.len()];
        self.next_idx += 1;
        Some(peer)
    }
}

/// Tracks which peers have advertised which transactions
#[derive(Debug)]
pub struct InvTracker {
    /// TX hash -> (ordered list of peers who INV'd it, peer index)
    sources: LruCache<[u8; 32], InvSource>,
}

impl InvTracker {
    pub fn new() -> Self {
        Self::with_capacity(INV_TRACKER_CAPACITY)
    }

    fn with_capacity(capacity: usize) -> Self {
        InvTracker {
            sources: LruCache::new(NonZeroUsize::new(capacity).unwrap()),
        }
    }

    /// Record that a peer has advertised a TX. Returns true if this is the first INV for this TX.
    pub fn record_source(&mut self, hash: [u8; 32], peer: PeerId) -> bool {
        let is_first = !self.sources.contains(&hash);

        let sources = self.sources.get_or_insert_mut(hash, InvSource::new);
        // Avoid duplicates
        if !sources.peers.contains(&peer) {
            sources.peers.push(peer);
        }

        is_first
    }

    /// Get the next peer to request from (round-robin)
    pub fn get_next_peer(&mut self, hash: &[u8; 32]) -> Option<PeerId> {
        self.sources.get_mut(hash)?.next_peer()
    }

    /// Peek at sources without updating LRU (for read-only access)
    pub fn peek_sources(&self, hash: &[u8; 32]) -> Option<&Vec<PeerId>> {
        self.sources.peek(hash).map(|source| &source.peers)
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
