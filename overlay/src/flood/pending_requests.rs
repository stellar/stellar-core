//! Pending GETDATA requests with timeout tracking.
//!
//! Tracks outstanding GETDATA requests and handles timeouts:
//! - 1 second timeout per peer
//! - 30 second total timeout before giving up
//! - Round-robin retry to different peers on timeout

use libp2p::PeerId;
use std::collections::HashMap;
use std::time::{Duration, Instant};

/// Timeout per GETDATA request to a single peer
pub const GETDATA_PEER_TIMEOUT: Duration = Duration::from_secs(1);

/// Total timeout before giving up on a TX
pub const GETDATA_TOTAL_TIMEOUT: Duration = Duration::from_secs(30);

/// A pending GETDATA request
#[derive(Debug, Clone)]
pub struct PendingRequest {
    /// Peer we sent GETDATA to
    pub peer: PeerId,
    /// When we sent the request
    pub sent_at: Instant,
    /// First request time (for total timeout)
    pub first_sent_at: Instant,
    /// Number of attempts so far
    pub attempts: u32,
}

impl PendingRequest {
    pub fn new(peer: PeerId) -> Self {
        let now = Instant::now();
        PendingRequest {
            peer,
            sent_at: now,
            first_sent_at: now,
            attempts: 1,
        }
    }

    /// Check if this request has timed out (per-peer timeout)
    pub fn is_timed_out(&self, timeout: Duration) -> bool {
        self.sent_at.elapsed() >= timeout
    }

    /// Check if we should give up entirely (total timeout)
    pub fn should_give_up(&self, total_timeout: Duration) -> bool {
        self.first_sent_at.elapsed() >= total_timeout
    }

    /// Update for retry to a new peer
    pub fn retry(&mut self, new_peer: PeerId) {
        self.peer = new_peer;
        self.sent_at = Instant::now();
        self.attempts += 1;
    }
}

/// Tracks pending GETDATA requests
#[derive(Debug)]
pub struct PendingRequests {
    /// TX hash -> pending request
    requests: HashMap<[u8; 32], PendingRequest>,
    /// Per-peer timeout
    peer_timeout: Duration,
    /// Total timeout
    total_timeout: Duration,
}

impl PendingRequests {
    pub fn new() -> Self {
        PendingRequests {
            requests: HashMap::new(),
            peer_timeout: GETDATA_PEER_TIMEOUT,
            total_timeout: GETDATA_TOTAL_TIMEOUT,
        }
    }

    /// Add a new pending request. Returns false if already pending.
    pub fn insert(&mut self, hash: [u8; 32], peer: PeerId) -> bool {
        if self.requests.contains_key(&hash) {
            return false;
        }
        self.requests.insert(hash, PendingRequest::new(peer));
        true
    }

    /// Remove a pending request (on successful completion)
    pub fn remove(&mut self, hash: &[u8; 32]) -> Option<PendingRequest> {
        self.requests.remove(hash)
    }

    /// Get mutable pending request (for retry)
    pub fn get_mut(&mut self, hash: &[u8; 32]) -> Option<&mut PendingRequest> {
        self.requests.get_mut(hash)
    }

    /// Process timeouts: returns (retry_list, give_up_list)
    /// - retry_list: hashes that timed out but haven't given up
    /// - give_up_list: hashes that should be abandoned
    pub fn process_timeouts(&self) -> (Vec<[u8; 32]>, Vec<[u8; 32]>) {
        let mut retry = Vec::new();
        let mut give_up = Vec::new();

        for (hash, req) in &self.requests {
            if req.should_give_up(self.total_timeout) {
                give_up.push(*hash);
            } else if req.is_timed_out(self.peer_timeout) {
                retry.push(*hash);
            }
        }

        (retry, give_up)
    }
}

impl Default for PendingRequests {
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
    fn test_pending_request_new() {
        let peer = PeerId::random();
        let req = PendingRequest::new(peer);

        assert_eq!(req.peer, peer);
        assert_eq!(req.attempts, 1);
        assert!(!req.is_timed_out(Duration::from_secs(1)));
    }

    #[test]
    fn test_pending_request_timeout() {
        let peer = PeerId::random();
        let mut req = PendingRequest::new(peer);
        req.sent_at = Instant::now() - Duration::from_secs(2);

        assert!(req.is_timed_out(Duration::from_secs(1)));
        assert!(!req.is_timed_out(Duration::from_secs(3)));
    }

    #[test]
    fn test_pending_request_give_up() {
        let peer = PeerId::random();
        let mut req = PendingRequest::new(peer);
        req.first_sent_at = Instant::now() - Duration::from_secs(31);

        assert!(req.should_give_up(Duration::from_secs(30)));
    }

    #[test]
    fn test_pending_request_retry() {
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();
        let mut req = PendingRequest::new(peer1);

        req.retry(peer2);

        assert_eq!(req.peer, peer2);
        assert_eq!(req.attempts, 2);
    }

    #[test]
    fn test_pending_requests_insert_remove() {
        let mut pending = PendingRequests::new();
        let peer = PeerId::random();

        assert!(pending.insert(hash(1), peer));

        // Can't insert duplicate
        assert!(!pending.insert(hash(1), peer));

        assert!(pending.remove(&hash(1)).is_some());
        assert!(pending.remove(&hash(1)).is_none());
    }

    #[test]
    fn test_pending_requests_process_timeouts() {
        let mut pending = PendingRequests::new();
        let peer = PeerId::random();

        pending.insert(hash(1), peer);

        // Make it look old
        pending.get_mut(&hash(1)).unwrap().sent_at = Instant::now() - Duration::from_secs(2);
        pending.get_mut(&hash(1)).unwrap().first_sent_at = Instant::now() - Duration::from_secs(2);

        // Should be in retry list, not give_up
        let (retry, give_up) = pending.process_timeouts();
        assert_eq!(retry.len(), 1);
        assert!(give_up.is_empty());

        // Make it even older
        pending.get_mut(&hash(1)).unwrap().first_sent_at = Instant::now() - Duration::from_secs(31);

        // Now should be in give_up list
        let (retry, give_up) = pending.process_timeouts();
        assert!(retry.is_empty()); // Not in retry if giving up
        assert_eq!(give_up.len(), 1);
    }
}
