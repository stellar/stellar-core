//! Bookkeeping for in-flight TX set fetches.
//!
//! A TX set referenced by an SCP message must be fetched from a peer before
//! Core can validate the round. A fetch is *pending* from the moment the
//! first `GetTxSet` request goes out until either a matching set arrives or
//! the slot it was needed for is far enough in the past that no peer would
//! still have it. While pending, a fetch is retried against another peer
//! whenever the current peer answers `DontHave`, disconnects, or stays silent
//! for longer than the retry timeout — so a single unlucky peer choice can
//! never stall the requester (Core issues each request once and relies on
//! the overlay to see it through).

use libp2p::PeerId;
use std::collections::HashMap;
use std::time::{Duration, Instant};

/// How long to wait for a peer to answer a `GetTxSet` before asking another.
/// Large sets take a while to transfer, so this is deliberately more relaxed
/// than the GETDATA per-peer timeout; a peer that does not have the set
/// answers `DontHave` immediately instead of running into this timeout.
pub const TXSET_FETCH_RETRY_TIMEOUT: Duration = Duration::from_secs(2);

/// Fetches for slots at least this many ledgers behind the current ledger are
/// abandoned. Matches the TX set cache eviction window, past which no peer
/// serves the set anyway.
pub const TXSET_FETCH_MAX_SLOT_LAG: u32 = 12;

/// One in-flight TX set fetch.
#[derive(Debug, Clone)]
pub struct PendingTxSetFetch {
    /// Peer the request is currently outstanding to
    pub peer: PeerId,
    /// Slot the set is needed for (stamps the cache entry, bounds the lifetime)
    pub slot: u32,
    /// When the first request for this set was sent
    pub first_requested_at: Instant,
    /// When the request to the current peer was sent
    pub sent_at: Instant,
    /// Number of requests sent so far
    pub attempts: u32,
    /// Peers already asked for this set (in order)
    tried: Vec<PeerId>,
    /// Set when the last send failed outright, so the next housekeeping tick
    /// re-dispatches without waiting for the timeout
    retry_now: bool,
}

impl PendingTxSetFetch {
    pub fn new(peer: PeerId, slot: u32) -> Self {
        let now = Instant::now();
        PendingTxSetFetch {
            peer,
            slot,
            first_requested_at: now,
            sent_at: now,
            attempts: 1,
            tried: vec![peer],
            retry_now: false,
        }
    }

    /// Has the current peer been silent for at least `timeout`?
    pub fn is_timed_out(&self, timeout: Duration) -> bool {
        self.retry_now || self.sent_at.elapsed() >= timeout
    }

    pub fn has_tried(&self, peer: &PeerId) -> bool {
        self.tried.contains(peer)
    }

    /// Re-assign the fetch to `peer` and restart the per-peer clock.
    pub fn retry(&mut self, peer: PeerId) {
        self.peer = peer;
        self.sent_at = Instant::now();
        self.attempts += 1;
        self.retry_now = false;
        if !self.tried.contains(&peer) {
            self.tried.push(peer);
        }
    }

    /// The send to the current peer failed before reaching the wire; make
    /// the fetch eligible for immediate re-dispatch.
    pub fn mark_send_failed(&mut self) {
        self.retry_now = true;
    }

    /// Nothing useful can be done right now (no peer connected, or the only
    /// candidate is the peer that just declined): restart the per-peer clock
    /// without sending, so the next attempt waits for a full retry timeout
    /// instead of spinning.
    pub fn defer(&mut self) {
        self.sent_at = Instant::now();
        self.retry_now = false;
    }

    /// Pick the peer to ask next, given the peer that announced the set (if
    /// any) and the currently connected peers.
    ///
    /// Preference order: the announcing peer if connected and not yet tried;
    /// any connected peer not yet tried; failing that, any connected peer
    /// other than the one currently being waited on (wrapping around); and if
    /// that is the only peer, that peer again. `None` only when nobody is
    /// connected.
    pub fn choose_next_peer(
        &self,
        preferred: Option<PeerId>,
        connected: &[PeerId],
    ) -> Option<PeerId> {
        if let Some(p) = preferred {
            if connected.contains(&p) && !self.has_tried(&p) {
                return Some(p);
            }
        }
        if let Some(p) = connected.iter().find(|p| !self.has_tried(p)) {
            return Some(*p);
        }
        connected
            .iter()
            .find(|p| **p != self.peer)
            .or_else(|| connected.iter().find(|p| **p == self.peer))
            .copied()
    }
}

/// All in-flight TX set fetches, keyed by TX set hash.
#[derive(Debug)]
pub struct PendingTxSetFetches {
    requests: HashMap<[u8; 32], PendingTxSetFetch>,
    timeout: Duration,
}

impl PendingTxSetFetches {
    pub fn new() -> Self {
        Self::with_timeout(TXSET_FETCH_RETRY_TIMEOUT)
    }

    pub fn with_timeout(timeout: Duration) -> Self {
        PendingTxSetFetches {
            requests: HashMap::new(),
            timeout,
        }
    }

    /// Override the per-peer retry timeout (tests).
    pub fn set_timeout(&mut self, timeout: Duration) {
        self.timeout = timeout;
    }

    pub fn timeout(&self) -> Duration {
        self.timeout
    }

    /// Record a new fetch. Returns false (and changes nothing) if one is
    /// already pending for this hash.
    pub fn insert(&mut self, hash: [u8; 32], peer: PeerId, slot: u32) -> bool {
        if self.requests.contains_key(&hash) {
            return false;
        }
        self.requests
            .insert(hash, PendingTxSetFetch::new(peer, slot));
        true
    }

    pub fn get(&self, hash: &[u8; 32]) -> Option<&PendingTxSetFetch> {
        self.requests.get(hash)
    }

    pub fn get_mut(&mut self, hash: &[u8; 32]) -> Option<&mut PendingTxSetFetch> {
        self.requests.get_mut(hash)
    }

    pub fn remove(&mut self, hash: &[u8; 32]) -> Option<PendingTxSetFetch> {
        self.requests.remove(hash)
    }

    /// Hashes whose current peer has been silent past the retry timeout.
    pub fn timed_out(&self) -> Vec<[u8; 32]> {
        self.requests
            .iter()
            .filter(|(_, req)| req.is_timed_out(self.timeout))
            .map(|(hash, _)| *hash)
            .collect()
    }

    /// Hashes currently outstanding to `peer`.
    pub fn assigned_to(&self, peer: &PeerId) -> Vec<[u8; 32]> {
        self.requests
            .iter()
            .filter(|(_, req)| req.peer == *peer)
            .map(|(hash, _)| *hash)
            .collect()
    }

    /// Drop fetches whose slot is at least `max_lag` ledgers behind
    /// `current_ledger`; returns what was dropped.
    pub fn prune_stale(&mut self, current_ledger: u32, max_lag: u32) -> Vec<[u8; 32]> {
        let mut dropped = Vec::new();
        self.requests.retain(|hash, req| {
            let stale = req.slot.saturating_add(max_lag) <= current_ledger;
            if stale {
                dropped.push(*hash);
            }
            !stale
        });
        dropped
    }

    pub fn len(&self) -> usize {
        self.requests.len()
    }

    pub fn is_empty(&self) -> bool {
        self.requests.is_empty()
    }
}

impl Default for PendingTxSetFetches {
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
    fn new_fetch_is_not_timed_out_and_tried_its_peer() {
        let peer = PeerId::random();
        let req = PendingTxSetFetch::new(peer, 7);
        assert_eq!(req.peer, peer);
        assert_eq!(req.slot, 7);
        assert_eq!(req.attempts, 1);
        assert!(req.has_tried(&peer));
        assert!(!req.is_timed_out(Duration::from_secs(1)));
    }

    #[test]
    fn timeout_uses_last_send_not_first_request() {
        let peer1 = PeerId::random();
        let peer2 = PeerId::random();
        let mut req = PendingTxSetFetch::new(peer1, 1);
        req.sent_at = Instant::now() - Duration::from_secs(5);
        req.first_requested_at = req.sent_at;
        assert!(req.is_timed_out(Duration::from_secs(2)));

        // Retrying restarts the per-peer clock but keeps the first-request
        // time (used for latency metrics).
        let first = req.first_requested_at;
        req.retry(peer2);
        assert!(!req.is_timed_out(Duration::from_secs(2)));
        assert_eq!(req.first_requested_at, first);
        assert_eq!(req.peer, peer2);
        assert_eq!(req.attempts, 2);
        assert!(req.has_tried(&peer1));
        assert!(req.has_tried(&peer2));
    }

    #[test]
    fn defer_restarts_clock_without_counting_an_attempt() {
        let peer = PeerId::random();
        let mut req = PendingTxSetFetch::new(peer, 1);
        req.sent_at = Instant::now() - Duration::from_secs(5);
        req.mark_send_failed();
        assert!(req.is_timed_out(Duration::from_secs(2)));

        req.defer();
        assert!(!req.is_timed_out(Duration::from_secs(2)));
        assert_eq!(req.attempts, 1);
        assert_eq!(req.peer, peer);
    }

    #[test]
    fn send_failure_makes_fetch_immediately_retryable() {
        let peer = PeerId::random();
        let mut req = PendingTxSetFetch::new(peer, 1);
        assert!(!req.is_timed_out(Duration::from_secs(60)));
        req.mark_send_failed();
        assert!(req.is_timed_out(Duration::from_secs(60)));
        req.retry(PeerId::random());
        assert!(!req.is_timed_out(Duration::from_secs(60)));
    }

    #[test]
    fn choose_next_peer_prefers_untried_source_then_untried_then_wraps() {
        let source = PeerId::random();
        let other = PeerId::random();
        let third = PeerId::random();
        let mut req = PendingTxSetFetch::new(other, 1);

        // Untried announcing peer wins over other untried peers.
        assert_eq!(
            req.choose_next_peer(Some(source), &[other, third, source]),
            Some(source)
        );
        req.retry(source);

        // Source tried: any untried connected peer.
        assert_eq!(
            req.choose_next_peer(Some(source), &[other, third, source]),
            Some(third)
        );
        req.retry(third);

        // Everyone tried: wrap to a peer other than the current one.
        let next = req
            .choose_next_peer(Some(source), &[other, third, source])
            .unwrap();
        assert_ne!(next, third);
        assert!(next == other || next == source);

        // Only the current peer is connected: ask it again rather than
        // giving up.
        assert_eq!(req.choose_next_peer(Some(source), &[third]), Some(third));

        // Nobody connected.
        assert_eq!(req.choose_next_peer(Some(source), &[]), None);
    }

    #[test]
    fn choose_next_peer_ignores_disconnected_source() {
        let source = PeerId::random();
        let other = PeerId::random();
        let req = PendingTxSetFetch::new(PeerId::random(), 1);
        assert_eq!(req.choose_next_peer(Some(source), &[other]), Some(other));
    }

    #[test]
    fn pending_fetches_insert_get_remove() {
        let mut pending = PendingTxSetFetches::new();
        let peer = PeerId::random();
        assert!(pending.insert(hash(1), peer, 10));
        // Second insert for the same hash is a no-op.
        assert!(!pending.insert(hash(1), PeerId::random(), 11));
        assert_eq!(pending.len(), 1);
        assert_eq!(pending.get(&hash(1)).unwrap().slot, 10);
        assert_eq!(pending.get(&hash(1)).unwrap().peer, peer);
        assert!(pending.remove(&hash(1)).is_some());
        assert!(pending.remove(&hash(1)).is_none());
        assert!(pending.is_empty());
    }

    #[test]
    fn timed_out_reports_only_silent_fetches() {
        let mut pending = PendingTxSetFetches::with_timeout(Duration::from_secs(2));
        pending.insert(hash(1), PeerId::random(), 1);
        pending.insert(hash(2), PeerId::random(), 1);
        pending.get_mut(&hash(1)).unwrap().sent_at = Instant::now() - Duration::from_secs(3);

        assert_eq!(pending.timed_out(), vec![hash(1)]);

        pending.get_mut(&hash(1)).unwrap().retry(PeerId::random());
        assert!(pending.timed_out().is_empty());
    }

    #[test]
    fn assigned_to_lists_fetches_outstanding_to_peer() {
        let mut pending = PendingTxSetFetches::new();
        let peer_a = PeerId::random();
        let peer_b = PeerId::random();
        pending.insert(hash(1), peer_a, 1);
        pending.insert(hash(2), peer_b, 1);
        pending.insert(hash(3), peer_a, 1);

        let mut to_a = pending.assigned_to(&peer_a);
        to_a.sort();
        assert_eq!(to_a, vec![hash(1), hash(3)]);
        assert_eq!(pending.assigned_to(&peer_b), vec![hash(2)]);
        assert!(pending.assigned_to(&PeerId::random()).is_empty());
    }

    #[test]
    fn prune_stale_drops_only_old_slots() {
        let mut pending = PendingTxSetFetches::new();
        pending.insert(hash(1), PeerId::random(), 100);
        pending.insert(hash(2), PeerId::random(), 88);
        pending.insert(hash(3), PeerId::random(), 89);

        // current = 100, lag = 12: slots <= 88 are stale.
        let dropped = pending.prune_stale(100, 12);
        assert_eq!(dropped, vec![hash(2)]);
        assert_eq!(pending.len(), 2);
        assert!(pending.get(&hash(2)).is_none());
        assert!(pending.get(&hash(3)).is_some());

        // A fetch for a future slot is never stale.
        pending.insert(hash(4), PeerId::random(), 1000);
        assert!(pending.prune_stale(100, 12).is_empty());
        assert!(pending.get(&hash(4)).is_some());

        // Boundary: exactly `max_lag` behind is stale, one less is not.
        let mut pending = PendingTxSetFetches::new();
        pending.insert(hash(5), PeerId::random(), 88);
        pending.insert(hash(6), PeerId::random(), 89);
        assert_eq!(pending.prune_stale(100, 12), vec![hash(5)]);
        assert!(pending.get(&hash(6)).is_some());
    }
}
