//! INV Tracker - tracks which peers have advertised which transactions.
//!
//! Used for:
//! 1. Round-robin GETDATA requests across peers who advertised a TX
//! 2. Avoiding sending INV back to peers who already know about a TX

use libp2p::PeerId;
use lru::LruCache;
use std::collections::HashSet;
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

    pub fn with_capacity(capacity: usize) -> Self {
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

    /// Get all peers who have advertised a TX
    pub fn get_sources(&mut self, hash: &[u8; 32]) -> Option<&Vec<PeerId>> {
        self.sources.get(hash)
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

    /// Get the next peer, excluding specific peers (e.g., already tried and failed)
    pub fn get_next_peer_excluding(
        &mut self,
        hash: &[u8; 32],
        exclude: &HashSet<PeerId>,
    ) -> Option<PeerId> {
        let sources = self.sources.get(hash)?;
        if sources.is_empty() {
            return None;
        }

        // Try all sources starting from current index
        let start_idx = *self.next_idx.get_or_insert(*hash, || 0);
        for offset in 0..sources.len() {
            let idx = (start_idx + offset) % sources.len();
            let peer = sources[idx];
            if !exclude.contains(&peer) {
                self.next_idx.put(*hash, idx + 1);
                return Some(peer);
            }
        }

        None // All sources are excluded
    }

    /// Check if we know about this TX (have at least one source)
    pub fn has_sources(&mut self, hash: &[u8; 32]) -> bool {
        self.sources
            .get(hash)
            .map(|s| !s.is_empty())
            .unwrap_or(false)
    }

    /// Number of sources for a TX
    pub fn source_count(&mut self, hash: &[u8; 32]) -> usize {
        self.sources.get(hash).map(|s| s.len()).unwrap_or(0)
    }

    /// Peek at sources without updating LRU (for read-only access)
    pub fn peek_sources(&self, hash: &[u8; 32]) -> Option<&Vec<PeerId>> {
        self.sources.peek(hash)
    }

    /// Remove a peer from all tracked TXs (on disconnect)
    pub fn remove_peer(&mut self, peer: &PeerId) {
        // This is O(n) over all tracked TXs - acceptable for disconnect events
        for (_, sources) in self.sources.iter_mut() {
            sources.retain(|p| p != peer);
        }
    }

    /// Clear tracking for a TX (e.g., after successful fetch)
    pub fn remove_tx(&mut self, hash: &[u8; 32]) {
        self.sources.pop(hash);
        self.next_idx.pop(hash);
    }

    /// Number of TXs being tracked
    pub fn len(&self) -> usize {
        self.sources.len()
    }

    pub fn is_empty(&self) -> bool {
        self.sources.is_empty()
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

        let sources = tracker.get_sources(&hash(1)).unwrap();
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

        let sources = tracker.get_sources(&hash(1)).unwrap();
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

        let sources = tracker.get_sources(&hash(1)).unwrap();
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

    #[test]
    fn test_inv_tracker_round_robin_excluding() {
        let mut tracker = InvTracker::new();
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();
        let peer3 = PeerId::random();

        tracker.record_source(hash(1), peer1);
        tracker.record_source(hash(1), peer2);
        tracker.record_source(hash(1), peer3);

        let mut exclude = HashSet::new();
        exclude.insert(peer1);

        // Should skip peer1
        let next = tracker.get_next_peer_excluding(&hash(1), &exclude);
        assert!(next == Some(peer2) || next == Some(peer3));
    }

    #[test]
    fn test_inv_tracker_all_excluded() {
        let mut tracker = InvTracker::new();
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();

        tracker.record_source(hash(1), peer1);
        tracker.record_source(hash(1), peer2);

        let mut exclude = HashSet::new();
        exclude.insert(peer1);
        exclude.insert(peer2);

        assert_eq!(tracker.get_next_peer_excluding(&hash(1), &exclude), None);
    }

    #[test]
    fn test_inv_tracker_remove_peer() {
        let mut tracker = InvTracker::new();
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();

        tracker.record_source(hash(1), peer1);
        tracker.record_source(hash(1), peer2);
        tracker.record_source(hash(2), peer1);

        tracker.remove_peer(&peer1);

        // peer1 removed from both TXs
        let sources1 = tracker.get_sources(&hash(1)).unwrap();
        assert_eq!(sources1.len(), 1);
        assert_eq!(sources1[0], peer2);

        let sources2 = tracker.get_sources(&hash(2)).unwrap();
        assert!(sources2.is_empty());
    }

    #[test]
    fn test_inv_tracker_remove_tx() {
        let mut tracker = InvTracker::new();
        let peer = PeerId::random();

        tracker.record_source(hash(1), peer);
        tracker.record_source(hash(2), peer);

        tracker.remove_tx(&hash(1));

        assert!(!tracker.has_sources(&hash(1)));
        assert!(tracker.has_sources(&hash(2)));
    }

    #[test]
    fn test_inv_tracker_has_sources() {
        let mut tracker = InvTracker::new();

        assert!(!tracker.has_sources(&hash(1)));

        tracker.record_source(hash(1), PeerId::random());
        assert!(tracker.has_sources(&hash(1)));
    }

    #[test]
    fn test_inv_tracker_source_count() {
        let mut tracker = InvTracker::new();

        assert_eq!(tracker.source_count(&hash(1)), 0);

        tracker.record_source(hash(1), PeerId::random());
        assert_eq!(tracker.source_count(&hash(1)), 1);

        tracker.record_source(hash(1), PeerId::random());
        assert_eq!(tracker.source_count(&hash(1)), 2);
    }

    #[test]
    fn test_inv_tracker_lru_eviction() {
        let mut tracker = InvTracker::with_capacity(3);

        tracker.record_source(hash(1), PeerId::random());
        tracker.record_source(hash(2), PeerId::random());
        tracker.record_source(hash(3), PeerId::random());

        assert_eq!(tracker.len(), 3);

        // Add 4th, should evict oldest (hash(1))
        tracker.record_source(hash(4), PeerId::random());

        assert_eq!(tracker.len(), 3);
        assert!(!tracker.has_sources(&hash(1))); // Evicted
        assert!(tracker.has_sources(&hash(4))); // New one present
    }

    #[test]
    fn test_inv_tracker_empty() {
        let tracker = InvTracker::new();
        assert!(tracker.is_empty());
        assert_eq!(tracker.len(), 0);
    }
}
