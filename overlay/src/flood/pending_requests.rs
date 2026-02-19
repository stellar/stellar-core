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

    /// Create with custom timeouts (for testing)
    pub fn with_timeouts(peer_timeout: Duration, total_timeout: Duration) -> Self {
        PendingRequests {
            requests: HashMap::new(),
            peer_timeout,
            total_timeout,
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

    /// Check if a request is pending
    pub fn is_pending(&self, hash: &[u8; 32]) -> bool {
        self.requests.contains_key(hash)
    }

    /// Get pending request info
    pub fn get(&self, hash: &[u8; 32]) -> Option<&PendingRequest> {
        self.requests.get(hash)
    }

    /// Get mutable pending request (for retry)
    pub fn get_mut(&mut self, hash: &[u8; 32]) -> Option<&mut PendingRequest> {
        self.requests.get_mut(hash)
    }

    /// Find all requests that have timed out (need retry or give up)
    pub fn timed_out(&self) -> Vec<[u8; 32]> {
        self.requests
            .iter()
            .filter(|(_, req)| req.is_timed_out(self.peer_timeout))
            .map(|(hash, _)| *hash)
            .collect()
    }

    /// Find all requests that should be abandoned
    pub fn abandoned(&self) -> Vec<[u8; 32]> {
        self.requests
            .iter()
            .filter(|(_, req)| req.should_give_up(self.total_timeout))
            .map(|(hash, _)| *hash)
            .collect()
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

    /// Remove all requests to a specific peer (on disconnect)
    pub fn remove_peer(&mut self, peer: &PeerId) -> Vec<[u8; 32]> {
        let affected: Vec<_> = self
            .requests
            .iter()
            .filter(|(_, req)| req.peer == *peer)
            .map(|(hash, _)| *hash)
            .collect();

        for hash in &affected {
            self.requests.remove(hash);
        }

        affected
    }

    /// Number of pending requests
    pub fn len(&self) -> usize {
        self.requests.len()
    }

    pub fn is_empty(&self) -> bool {
        self.requests.is_empty()
    }

    /// Time until next timeout (for timer scheduling)
    pub fn time_until_next_timeout(&self) -> Option<Duration> {
        self.requests
            .values()
            .map(|req| {
                let elapsed = req.sent_at.elapsed();
                if elapsed >= self.peer_timeout {
                    Duration::ZERO
                } else {
                    self.peer_timeout - elapsed
                }
            })
            .min()
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
        assert!(pending.is_pending(&hash(1)));

        // Can't insert duplicate
        assert!(!pending.insert(hash(1), peer));

        pending.remove(&hash(1));
        assert!(!pending.is_pending(&hash(1)));
    }

    #[test]
    fn test_pending_requests_timed_out() {
        let mut pending =
            PendingRequests::with_timeouts(Duration::from_millis(50), Duration::from_secs(30));
        let peer = PeerId::random();

        pending.insert(hash(1), peer);
        pending.insert(hash(2), peer);

        // None timed out yet
        assert!(pending.timed_out().is_empty());

        // Wait for timeout
        std::thread::sleep(Duration::from_millis(60));

        let timed_out = pending.timed_out();
        assert_eq!(timed_out.len(), 2);
    }

    #[test]
    fn test_pending_requests_process_timeouts() {
        let mut pending =
            PendingRequests::with_timeouts(Duration::from_millis(10), Duration::from_millis(50));
        let peer = PeerId::random();

        pending.insert(hash(1), peer);

        // Make it look old
        pending.get_mut(&hash(1)).unwrap().sent_at = Instant::now() - Duration::from_millis(20);
        pending.get_mut(&hash(1)).unwrap().first_sent_at =
            Instant::now() - Duration::from_millis(20);

        // Should be in retry list, not give_up
        let (retry, give_up) = pending.process_timeouts();
        assert_eq!(retry.len(), 1);
        assert!(give_up.is_empty());

        // Make it even older
        pending.get_mut(&hash(1)).unwrap().first_sent_at =
            Instant::now() - Duration::from_millis(60);

        // Now should be in give_up list
        let (retry, give_up) = pending.process_timeouts();
        assert!(retry.is_empty()); // Not in retry if giving up
        assert_eq!(give_up.len(), 1);
    }

    #[test]
    fn test_pending_requests_remove_peer() {
        let mut pending = PendingRequests::new();
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();

        pending.insert(hash(1), peer1);
        pending.insert(hash(2), peer1);
        pending.insert(hash(3), peer2);

        let removed = pending.remove_peer(&peer1);
        assert_eq!(removed.len(), 2);
        assert!(!pending.is_pending(&hash(1)));
        assert!(!pending.is_pending(&hash(2)));
        assert!(pending.is_pending(&hash(3)));
    }

    #[test]
    fn test_pending_requests_time_until_timeout() {
        let mut pending =
            PendingRequests::with_timeouts(Duration::from_millis(100), Duration::from_secs(30));

        // No requests
        assert!(pending.time_until_next_timeout().is_none());

        let peer = PeerId::random();
        pending.insert(hash(1), peer);

        // Should be ~100ms until timeout
        let remaining = pending.time_until_next_timeout().unwrap();
        assert!(remaining <= Duration::from_millis(100));
        assert!(remaining >= Duration::from_millis(90));
    }

    #[test]
    fn test_pending_requests_len() {
        let mut pending = PendingRequests::new();

        assert!(pending.is_empty());
        assert_eq!(pending.len(), 0);

        pending.insert(hash(1), PeerId::random());
        pending.insert(hash(2), PeerId::random());

        assert!(!pending.is_empty());
        assert_eq!(pending.len(), 2);
    }
}
