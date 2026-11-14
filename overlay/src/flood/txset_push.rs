//! Bookkeeping for the leader's tx-set push.
//!
//! A round's leader sends its tx set to every connected peer as soon as the
//! set is built, without waiting for a request. The pushed frame is the same
//! frame a request is answered with, and the receiver verifies and files it by
//! its own content hash, so nothing here is trusted: these types only decide
//! what to send, what to skip and how much unrequested data to keep.
//!
//! * [`PushRecord`] (sender): which peers a set went to, so that it is pushed
//!   once per peer and a request that crosses the push is not answered with a
//!   second copy.
//! * [`PushRateLimiter`] (receiver): how many tx-set frames a peer may send
//!   beyond the answers to this node's requests before they are dropped
//!   undecoded.
//! * [`UnclaimedSetBudget`] (receiver): how many sets from peers stay cached
//!   while Core has not asked for them.

use super::Hash256;
use libp2p::PeerId;
use std::collections::{HashMap, VecDeque};
use std::num::NonZeroUsize;
use std::time::{Duration, Instant};

/// Sets a node remembers having pushed. A node pushes one set per round it
/// leads, and a request can only cross a push for a few round trips.
const PUSH_RECORD_SETS: usize = 32;

/// How long after a push was written a request from that peer is still taken
/// to have crossed it. The peer asks when the proposal arrives, which the
/// leader sends about 70 ms after the push, so its request reaches the leader
/// about 70 ms plus one round trip after the push was written. A later
/// request means the peer does not hold the set (it only asks on a cache
/// miss) and is answered.
pub const PUSH_SKIP_WINDOW: Duration = Duration::from_millis(500);

/// Pushes that may be queued or in flight to one peer at a time. A peer that
/// stops reading its tx-set stream then holds at most this many sets in
/// memory; further sets are not pushed to it and it requests them instead.
pub const MAX_IN_FLIGHT_PUSHES_PER_PEER: usize = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PushProgress {
    /// Queued on, or being written to, the peer's tx-set stream.
    InFlight,
    /// Handed to the transport at this time.
    Written(Instant),
}

#[derive(Clone, Copy, Debug)]
struct PeerPush {
    progress: PushProgress,
    /// The peer's first request for the set was skipped.
    asked: bool,
}

/// Sender side: the sets this node pushed, per peer, on the peer's current
/// connection. Entries for a peer are dropped when a connection to it closes.
pub struct PushRecord {
    by_hash: lru::LruCache<Hash256, HashMap<PeerId, PeerPush>>,
    /// Pushes not yet handed to the transport, per peer.
    in_flight: HashMap<PeerId, usize>,
}

impl Default for PushRecord {
    fn default() -> Self {
        Self::new()
    }
}

impl PushRecord {
    pub fn new() -> Self {
        Self {
            by_hash: lru::LruCache::new(NonZeroUsize::new(PUSH_RECORD_SETS).unwrap()),
            in_flight: HashMap::new(),
        }
    }

    /// Record a push of `hash` and return the peers among `connected` to send
    /// it to: those that have not been sent it yet and have no other push
    /// still in flight. A repeated push only reaches peers that connected
    /// since. A peer that is skipped because it is busy is not recorded, so
    /// its request for the set is answered.
    pub fn start_push(
        &mut self,
        hash: Hash256,
        connected: impl IntoIterator<Item = PeerId>,
    ) -> Vec<PeerId> {
        self.start_push_counting_busy(hash, connected).0
    }

    /// As [`Self::start_push`]; also returns how many peers were left out
    /// because an earlier push to them is still in flight.
    pub fn start_push_counting_busy(
        &mut self,
        hash: Hash256,
        connected: impl IntoIterator<Item = PeerId>,
    ) -> (Vec<PeerId>, usize) {
        let (targets, busy) = self.start_push_listing_busy(hash, connected);
        (targets, busy.len())
    }

    /// As [`Self::start_push`]; also returns the peers that were left out
    /// because an earlier push to them is still in flight.
    pub fn start_push_listing_busy(
        &mut self,
        hash: Hash256,
        connected: impl IntoIterator<Item = PeerId>,
    ) -> (Vec<PeerId>, Vec<PeerId>) {
        let peers = self.by_hash.get_or_insert_mut(hash, HashMap::new);
        let mut targets = Vec::new();
        let mut busy = Vec::new();
        for peer in connected {
            if peers.contains_key(&peer) {
                continue;
            }
            let in_flight = self.in_flight.entry(peer).or_insert(0);
            if *in_flight >= MAX_IN_FLIGHT_PUSHES_PER_PEER {
                busy.push(peer);
                continue;
            }
            *in_flight += 1;
            peers.insert(
                peer,
                PeerPush {
                    progress: PushProgress::InFlight,
                    asked: false,
                },
            );
            targets.push(peer);
        }
        (targets, busy)
    }

    fn finish_in_flight(&mut self, peer: &PeerId) {
        if let Some(count) = self.in_flight.get_mut(peer) {
            *count = count.saturating_sub(1);
            if *count == 0 {
                self.in_flight.remove(peer);
            }
        }
    }

    /// The push to `peer` was handed to the transport.
    pub fn push_written(&mut self, hash: &Hash256, peer: &PeerId, now: Instant) {
        self.finish_in_flight(peer);
        if let Some(push) = self
            .by_hash
            .peek_mut(hash)
            .and_then(|peers| peers.get_mut(peer))
        {
            push.progress = PushProgress::Written(now);
        }
    }

    /// The write to `peer` failed, so the peer does not have the set from us.
    /// Returns true if a request from that peer was skipped because of the
    /// push; the caller then answers it after all.
    pub fn push_failed(&mut self, hash: &Hash256, peer: &PeerId) -> bool {
        self.finish_in_flight(peer);
        self.by_hash
            .peek_mut(hash)
            .and_then(|peers| peers.remove(peer))
            .is_some_and(|push| push.asked)
    }

    /// `peer` asked for `hash`. Returns true if the request should not be
    /// answered: the set is still being written to that peer, or was written
    /// less than [`PUSH_SKIP_WINDOW`] ago, on the same ordered, reliable
    /// stream an answer would use, so a second copy could only arrive later.
    ///
    /// Only the first such request is skipped. A repeated request, or one
    /// that arrives after the window, means the peer does not hold the pushed
    /// copy (it was dropped by the peer's limits, or evicted) and is answered.
    pub fn should_skip_request(&mut self, hash: &Hash256, peer: &PeerId, now: Instant) -> bool {
        let Some(push) = self
            .by_hash
            .peek_mut(hash)
            .and_then(|peers| peers.get_mut(peer))
        else {
            return false;
        };
        if push.asked {
            return false;
        }
        let crossing = match push.progress {
            PushProgress::InFlight => true,
            PushProgress::Written(at) => now.saturating_duration_since(at) < PUSH_SKIP_WINDOW,
        };
        if crossing {
            push.asked = true;
        }
        crossing
    }

    /// A connection to the peer closed: the pushed sets may have been on its
    /// streams and not arrived, so the peer's requests are answered again.
    ///
    /// The in-flight count is left alone. Every push task ends in
    /// `push_written` or `push_failed` (a write on a closed connection fails),
    /// so the count stays exact; resetting it here would let a peer that does
    /// not read its stream collect one stuck push per connection it closes.
    pub fn forget_peer(&mut self, peer: &PeerId) {
        for (_, peers) in self.by_hash.iter_mut() {
            peers.remove(peer);
        }
    }

    #[cfg(test)]
    pub fn was_pushed(&self, hash: &Hash256, peer: &PeerId) -> bool {
        self.by_hash
            .peek(hash)
            .is_some_and(|peers| peers.contains_key(peer))
    }
}

/// Unrequested tx-set frames a peer may send in a burst.
pub const PUSH_BURST: f64 = 8.0;
/// Sustained rate of unrequested tx-set frames per peer, per second. An honest
/// leader pushes one set per round it leads (two if it also prepared one).
pub const PUSH_REFILL_PER_SEC: f64 = 4.0;
/// Frames beyond the rate that a peer may still send because this node sent
/// it tx-set requests: one per request, at most this many saved up.
pub const MAX_REQUEST_CREDITS: u32 = 8;

/// Peers the rate limiter tracks before it starts over.
const MAX_TRACKED_PEERS: usize = 4096;

struct PeerAllowance {
    tokens: f64,
    last_refill: Instant,
    /// Requests this node sent the peer that have not been used up by a
    /// frame over the rate.
    request_credits: u32,
}

/// Receiver side: how many tx-set frames a peer may have decoded.
///
/// Every frame takes a token from the peer's bucket; a frame that turns out
/// to answer a request of this node gives it back, so answers are free and
/// only frames nobody asked for use the allowance. With the bucket empty, a
/// frame is still let through if this node sent that peer a request it has
/// not used up (the frame may be the answer). Anything else is dropped before
/// it is decompressed.
///
/// What this does not bound: a peer can make this node send it requests (the
/// overlay prefetches any hash named in an SCP envelope, before Core checks
/// the signature), so it can buy one extra frame per envelope.
pub struct PushRateLimiter {
    peers: HashMap<PeerId, PeerAllowance>,
    burst: f64,
    refill_per_sec: f64,
}

impl Default for PushRateLimiter {
    fn default() -> Self {
        Self::new(PUSH_BURST, PUSH_REFILL_PER_SEC)
    }
}

impl PushRateLimiter {
    pub fn new(burst: f64, refill_per_sec: f64) -> Self {
        Self {
            peers: HashMap::new(),
            burst,
            refill_per_sec,
        }
    }

    fn allowance(&mut self, peer: PeerId, now: Instant) -> &mut PeerAllowance {
        let burst = self.burst;
        let refill = self.refill_per_sec;
        // Entries go when a peer disconnects. A reader task that is still
        // draining can recreate one afterwards; keep the map bounded anyway.
        if self.peers.len() >= MAX_TRACKED_PEERS && !self.peers.contains_key(&peer) {
            self.peers.clear();
        }
        let allowance = self.peers.entry(peer).or_insert(PeerAllowance {
            tokens: burst,
            last_refill: now,
            request_credits: 0,
        });
        let elapsed = now
            .saturating_duration_since(allowance.last_refill)
            .as_secs_f64();
        allowance.tokens = (allowance.tokens + elapsed * refill).min(burst);
        allowance.last_refill = now;
        allowance
    }

    /// A tx-set frame arrived from `peer`. False means it is over the limit
    /// and must be dropped undecoded.
    pub fn allow(&mut self, peer: PeerId, now: Instant) -> bool {
        let allowance = self.allowance(peer, now);
        if allowance.tokens >= 1.0 {
            allowance.tokens -= 1.0;
            true
        } else if allowance.request_credits > 0 {
            allowance.request_credits -= 1;
            true
        } else {
            false
        }
    }

    /// This node sent `peer` a tx-set request.
    pub fn request_sent(&mut self, peer: PeerId, now: Instant) {
        let allowance = self.allowance(peer, now);
        allowance.request_credits = (allowance.request_credits + 1).min(MAX_REQUEST_CREDITS);
    }

    /// A frame that was let through answered a request of this node: it does
    /// not count against the peer.
    pub fn answered_request(&mut self, peer: PeerId, now: Instant) {
        let burst = self.burst;
        let allowance = self.allowance(peer, now);
        allowance.tokens = (allowance.tokens + 1.0).min(burst);
    }

    /// The peer's last connection closed. A peer that reconnects starts with
    /// a full allowance again.
    pub fn forget_peer(&mut self, peer: &PeerId) {
        self.peers.remove(peer);
    }

    #[cfg(test)]
    pub fn request_credits(&self, peer: &PeerId) -> u32 {
        self.peers.get(peer).map_or(0, |a| a.request_credits)
    }
}

/// Unclaimed sets kept per peer that the peer sent unrequested (pushes).
pub const MAX_UNCLAIMED_SETS_PER_PEER: usize = 4;
/// Unclaimed sets kept per peer that answered this node's own requests
/// (prefetches for hashes named in SCP envelopes). A peer answers a new
/// connection's SCP state request with up to about 12 slots of envelopes, and
/// the overlay may receive all of their sets before Core asks for the first.
pub const MAX_UNCLAIMED_REQUESTED_SETS_PER_PEER: usize = 16;
/// Bytes of unclaimed sets kept in total (XDR plus encoding).
pub const MAX_UNCLAIMED_BYTES: usize = 128 * 1024 * 1024;

/// Receiver side: the cached sets that came from a peer and that Core has not
/// asked for ("unclaimed"): pushed sets, and sets the overlay prefetched
/// because an SCP envelope named them. A set leaves this budget when Core
/// requests it or builds it (it is then an ordinary cache entry) or when the
/// cache drops it.
///
/// Limits: a fixed number of sets per peer, oldest dropped first, and one byte
/// budget over all peers. Over the byte budget the peer holding the most bytes
/// loses its oldest set, so a few peers cannot push out the sets of the
/// others. (Many colluding peers can: the budget then evens out the holders,
/// and a large honest set can go. Its receiver falls back to a request.)
pub struct UnclaimedSetBudget {
    /// Per peer, oldest first: (hash, bytes, arrived unrequested).
    per_peer: HashMap<PeerId, VecDeque<(Hash256, usize, bool)>>,
    /// Hash -> (peer it came from, whether it arrived unrequested).
    owner: HashMap<Hash256, (PeerId, bool)>,
    total_bytes: usize,
    max_pushed_per_peer: usize,
    max_requested_per_peer: usize,
    max_total_bytes: usize,
}

impl Default for UnclaimedSetBudget {
    fn default() -> Self {
        Self::new(
            MAX_UNCLAIMED_SETS_PER_PEER,
            MAX_UNCLAIMED_REQUESTED_SETS_PER_PEER,
            MAX_UNCLAIMED_BYTES,
        )
    }
}

impl UnclaimedSetBudget {
    pub fn new(
        max_pushed_per_peer: usize,
        max_requested_per_peer: usize,
        max_total_bytes: usize,
    ) -> Self {
        Self {
            per_peer: HashMap::new(),
            owner: HashMap::new(),
            total_bytes: 0,
            max_pushed_per_peer,
            max_requested_per_peer,
            max_total_bytes,
        }
    }

    /// Track a set of `bytes` from `peer` that Core has not asked for.
    /// `pushed` says whether it arrived unrequested; pushed and requested
    /// sets have separate per-peer limits. Returns the hashes the caller must
    /// drop from its cache to stay within the limits; that can include `hash`
    /// itself.
    pub fn insert(
        &mut self,
        peer: PeerId,
        hash: Hash256,
        bytes: usize,
        pushed: bool,
    ) -> Vec<Hash256> {
        if self.owner.contains_key(&hash) {
            return Vec::new();
        }
        self.owner.insert(hash, (peer, pushed));
        self.per_peer
            .entry(peer)
            .or_default()
            .push_back((hash, bytes, pushed));
        self.total_bytes += bytes;

        let limit = if pushed {
            self.max_pushed_per_peer
        } else {
            self.max_requested_per_peer
        };
        let mut evicted = Vec::new();
        while self.count_of_kind(&peer, pushed) > limit {
            evicted.extend(self.evict_oldest_of(&peer, Some(pushed)));
        }
        while self.total_bytes > self.max_total_bytes {
            let Some(largest) = self
                .per_peer
                .iter()
                .max_by_key(|(_, sets)| sets.iter().map(|(_, bytes, _)| *bytes).sum::<usize>())
                .map(|(peer, _)| *peer)
            else {
                break;
            };
            evicted.extend(self.evict_oldest_of(&largest, None));
        }
        evicted
    }

    fn count_of_kind(&self, peer: &PeerId, pushed: bool) -> usize {
        self.per_peer.get(peer).map_or(0, |sets| {
            sets.iter().filter(|(_, _, kind)| *kind == pushed).count()
        })
    }

    /// Drop the peer's oldest set, of the given kind if one is named.
    fn evict_oldest_of(&mut self, peer: &PeerId, kind: Option<bool>) -> Option<Hash256> {
        let sets = self.per_peer.get_mut(peer)?;
        let pos = sets
            .iter()
            .position(|(_, _, pushed)| kind.map_or(true, |kind| kind == *pushed))?;
        let (hash, bytes, _) = sets.remove(pos)?;
        if sets.is_empty() {
            self.per_peer.remove(peer);
        }
        self.owner.remove(&hash);
        self.total_bytes -= bytes;
        Some(hash)
    }

    /// Stop counting `hash`: Core asked for it or built it, or the cache
    /// dropped it. Returns `Some(pushed)` if it was an unclaimed set, where
    /// `pushed` says whether it had arrived unrequested.
    pub fn claim(&mut self, hash: &Hash256) -> Option<bool> {
        let (peer, pushed) = self.owner.remove(hash)?;
        if let Some(sets) = self.per_peer.get_mut(&peer) {
            if let Some(pos) = sets.iter().position(|(h, _, _)| h == hash) {
                let (_, bytes, _) = sets.remove(pos).unwrap();
                self.total_bytes -= bytes;
            }
            if sets.is_empty() {
                self.per_peer.remove(&peer);
            }
        }
        Some(pushed)
    }

    /// Forget every set for which `keep` is false (the cache evicted it).
    pub fn retain(&mut self, keep: impl Fn(&Hash256) -> bool) {
        let gone: Vec<Hash256> = self.owner.keys().filter(|h| !keep(h)).copied().collect();
        for hash in gone {
            self.claim(&hash);
        }
    }

    pub fn contains(&self, hash: &Hash256) -> bool {
        self.owner.contains_key(hash)
    }

    pub fn len(&self) -> usize {
        self.owner.len()
    }

    pub fn is_empty(&self) -> bool {
        self.owner.is_empty()
    }

    pub fn total_bytes(&self) -> usize {
        self.total_bytes
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn peer() -> PeerId {
        PeerId::random()
    }

    fn written(record: &mut PushRecord, hash: Hash256, peers: &[PeerId], now: Instant) {
        for peer in peers {
            record.push_written(&hash, peer, now);
        }
    }

    #[test]
    fn push_goes_to_each_peer_once() {
        let mut record = PushRecord::new();
        let (a, b, c) = (peer(), peer(), peer());
        let hash = [1u8; 32];
        let now = Instant::now();

        let mut first = record.start_push(hash, [a, b]);
        first.sort();
        let mut expected = vec![a, b];
        expected.sort();
        assert_eq!(first, expected);
        written(&mut record, hash, &[a, b], now);

        // A repeated push reaches only the peer that connected since.
        assert_eq!(record.start_push(hash, [a, b, c]), vec![c]);
        written(&mut record, hash, &[c], now);
        assert!(record.start_push(hash, [a, b, c]).is_empty());

        // Another set is independent.
        assert_eq!(record.start_push([2u8; 32], [a]), vec![a]);
    }

    #[test]
    fn a_peer_with_a_push_in_flight_is_not_sent_another() {
        let mut record = PushRecord::new();
        let (stuck, fine) = (peer(), peer());
        let now = Instant::now();
        let (first, second, third) = ([1u8; 32], [2u8; 32], [3u8; 32]);

        let mut targets = record.start_push(first, [stuck, fine]);
        targets.sort();
        assert_eq!(targets.len(), 2);
        // Only `fine` finishes its write; `stuck` does not read.
        record.push_written(&first, &fine, now);

        // The next set goes to `fine` only, and `stuck` is not recorded for
        // it: its request for that set is answered through the request path.
        assert_eq!(record.start_push(second, [stuck, fine]), vec![fine]);
        assert!(!record.was_pushed(&second, &stuck));
        assert!(!record.should_skip_request(&second, &stuck, now));
        record.push_written(&second, &fine, now);

        // Once the stuck write ends (here: fails), pushes resume.
        record.push_failed(&first, &stuck);
        let mut targets = record.start_push(third, [stuck, fine]);
        targets.sort();
        assert_eq!(targets.len(), 2);
    }

    #[test]
    fn a_crossing_request_is_skipped_once() {
        let mut record = PushRecord::new();
        let (a, b) = (peer(), peer());
        let hash = [3u8; 32];
        let now = Instant::now();
        record.start_push(hash, [a]);

        // Not pushed to b, and not this hash to a: answer.
        assert!(!record.should_skip_request(&hash, &b, now));
        assert!(!record.should_skip_request(&[4u8; 32], &a, now));

        // In flight to a: the first request is skipped, a repeat is answered.
        assert!(record.should_skip_request(&hash, &a, now));
        assert!(!record.should_skip_request(&hash, &a, now));
        assert!(!record.should_skip_request(&hash, &a, now));
    }

    #[test]
    fn a_request_long_after_the_push_is_answered() {
        let mut record = PushRecord::new();
        let (early, late, in_flight) = (peer(), peer(), peer());
        let hash = [6u8; 32];
        let start = Instant::now();
        record.start_push(hash, [early, late, in_flight]);
        written(&mut record, hash, &[early, late], start);

        // Inside the window the request crossed the push.
        let inside = start + PUSH_SKIP_WINDOW - Duration::from_millis(1);
        assert!(record.should_skip_request(&hash, &early, inside));

        // After the window the peer evidently does not hold the pushed copy
        // (it lost or dropped it): the request is answered, every time.
        let after = start + PUSH_SKIP_WINDOW;
        assert!(!record.should_skip_request(&hash, &late, after));
        assert!(!record.should_skip_request(&hash, &late, after));

        // A push that is still being written is skipped whatever the time.
        let much_later = start + Duration::from_secs(60);
        assert!(record.should_skip_request(&hash, &in_flight, much_later));
    }

    #[test]
    fn failed_push_and_disconnect_restore_the_request_path() {
        let mut record = PushRecord::new();
        let (a, b) = (peer(), peer());
        let hash = [5u8; 32];
        let now = Instant::now();
        record.start_push(hash, [a, b]);

        // No request was skipped for a: nothing to answer after the failure.
        assert!(!record.push_failed(&hash, &a));
        assert!(!record.was_pushed(&hash, &a));
        assert!(!record.should_skip_request(&hash, &a, now));
        // The set can be pushed to a again.
        assert_eq!(record.start_push(hash, [a, b]), vec![a]);

        // b asked while the push was in flight, then the push failed: the
        // caller has to answer the skipped request.
        assert!(record.should_skip_request(&hash, &b, now));
        assert!(record.push_failed(&hash, &b));
        assert!(!record.push_failed(&hash, &b));

        record.forget_peer(&a);
        assert!(!record.was_pushed(&hash, &a));
        assert!(!record.should_skip_request(&hash, &a, now));
    }

    #[test]
    fn a_closed_connection_does_not_release_a_push_still_in_flight() {
        let mut record = PushRecord::new();
        let a = peer();
        let now = Instant::now();
        let (first, second, third) = ([1u8; 32], [2u8; 32], [3u8; 32]);
        assert_eq!(record.start_push(first, [a]), vec![a]);

        // A connection to the peer closes (possibly a duplicate one) while
        // the write is still stuck: the peer's requests are answered again,
        // but it is not sent another set until that write has ended.
        record.forget_peer(&a);
        assert!(!record.should_skip_request(&first, &a, now));
        assert!(record.start_push(second, [a]).is_empty());
        assert!(record.start_push(second, [a]).is_empty());

        // The stuck write ends (on a closed connection it fails). Its
        // completion is counted once, even though the record was cleared.
        assert!(!record.push_failed(&first, &a));
        assert!(record.in_flight.is_empty());
        assert_eq!(record.start_push(third, [a]), vec![a]);
        record.push_written(&third, &a, now);
        assert!(record.in_flight.is_empty());
    }

    #[test]
    fn push_record_is_bounded() {
        let mut record = PushRecord::new();
        let a = peer();
        let now = Instant::now();
        for i in 0..(PUSH_RECORD_SETS as u32 + 10) {
            let mut hash = [0u8; 32];
            hash[..4].copy_from_slice(&i.to_be_bytes());
            assert_eq!(record.start_push(hash, [a]), vec![a]);
            record.push_written(&hash, &a, now);
        }
        assert_eq!(record.by_hash.len(), PUSH_RECORD_SETS);
        assert!(record.in_flight.is_empty());
        // The oldest sets fell out: requests for them are answered normally.
        assert!(!record.should_skip_request(&[0u8; 32], &a, now));
    }

    #[test]
    fn rate_limiter_allows_a_burst_then_the_sustained_rate() {
        let mut limiter = PushRateLimiter::new(3.0, 2.0);
        let (a, b) = (peer(), peer());
        let start = Instant::now();

        assert!(limiter.allow(a, start));
        assert!(limiter.allow(a, start));
        assert!(limiter.allow(a, start));
        assert!(!limiter.allow(a, start));
        // Peers are independent.
        assert!(limiter.allow(b, start));

        // Half a second refills one token at 2 per second.
        let later = start + Duration::from_millis(500);
        assert!(limiter.allow(a, later));
        assert!(!limiter.allow(a, later));

        // A long pause refills to the burst, not beyond it.
        let much_later = later + Duration::from_secs(3600);
        for _ in 0..3 {
            assert!(limiter.allow(a, much_later));
        }
        assert!(!limiter.allow(a, much_later));

        limiter.forget_peer(&a);
        assert!(limiter.allow(a, much_later));
    }

    #[test]
    fn answers_to_requests_do_not_use_the_allowance() {
        let mut limiter = PushRateLimiter::new(2.0, 1.0);
        let a = peer();
        let now = Instant::now();

        // Many answered requests in a row: each frame is let through and
        // gives its token back.
        for _ in 0..50 {
            limiter.request_sent(a, now);
            assert!(limiter.allow(a, now));
            limiter.answered_request(a, now);
        }
        // The allowance for unrequested frames is intact ...
        assert!(limiter.allow(a, now));
        assert!(limiter.allow(a, now));
        // ... and once it is used up, the saved-up requests (capped) let
        // that many more frames through, which may be late answers.
        for _ in 0..MAX_REQUEST_CREDITS {
            assert!(limiter.allow(a, now));
        }
        assert!(!limiter.allow(a, now));

        // One request buys exactly one frame over the rate.
        limiter.request_sent(a, now);
        assert!(limiter.allow(a, now));
        assert!(!limiter.allow(a, now));
    }

    #[test]
    fn rate_limiter_tolerates_a_clock_that_does_not_advance() {
        let mut limiter = PushRateLimiter::new(1.0, 1.0);
        let a = peer();
        let now = Instant::now();
        assert!(limiter.allow(a, now));
        // An earlier instant must not underflow or add tokens.
        assert!(!limiter.allow(a, now - Duration::from_millis(1)));
    }

    #[test]
    fn budget_keeps_a_fixed_number_of_sets_per_peer() {
        let mut budget = UnclaimedSetBudget::new(2, 3, 1_000_000);
        let (a, b) = (peer(), peer());

        assert!(budget.insert(a, [1u8; 32], 10, true).is_empty());
        assert!(budget.insert(a, [2u8; 32], 10, true).is_empty());
        // The third set from a pushes out a's oldest, not b's.
        assert!(budget.insert(b, [9u8; 32], 10, true).is_empty());
        assert_eq!(budget.insert(a, [3u8; 32], 10, true), vec![[1u8; 32]]);

        assert!(!budget.contains(&[1u8; 32]));
        assert!(budget.contains(&[2u8; 32]));
        assert!(budget.contains(&[3u8; 32]));
        assert!(budget.contains(&[9u8; 32]));
        assert_eq!(budget.total_bytes(), 30);
        assert_eq!(budget.len(), 3);
    }

    #[test]
    fn pushed_and_requested_sets_have_separate_per_peer_limits() {
        // 2 pushed and 3 requested sets per peer.
        let mut budget = UnclaimedSetBudget::new(2, 3, 1_000_000);
        let a = peer();
        // Answers to this node's own requests (a burst after a reconnect).
        for i in 0..3u8 {
            assert!(budget.insert(a, [i; 32], 10, false).is_empty());
        }
        // Pushes from the same peer do not evict them, and the other way
        // round: each kind only pushes out its own oldest.
        assert!(budget.insert(a, [10u8; 32], 10, true).is_empty());
        assert!(budget.insert(a, [11u8; 32], 10, true).is_empty());
        assert_eq!(budget.insert(a, [12u8; 32], 10, true), vec![[10u8; 32]]);
        assert_eq!(budget.insert(a, [3u8; 32], 10, false), vec![[0u8; 32]]);
        assert_eq!(budget.len(), 5);
        assert_eq!(budget.total_bytes(), 50);
    }

    #[test]
    fn byte_budget_evicts_from_the_largest_holder() {
        let mut budget = UnclaimedSetBudget::new(4, 4, 100);
        let (honest, greedy) = (peer(), peer());

        assert!(budget.insert(honest, [1u8; 32], 10, true).is_empty());
        assert!(budget.insert(greedy, [2u8; 32], 40, true).is_empty());
        assert!(budget.insert(greedy, [3u8; 32], 40, true).is_empty());
        // 130 bytes > 100: the greedy peer loses its own oldest set.
        assert_eq!(budget.insert(greedy, [4u8; 32], 40, true), vec![[2u8; 32]]);
        assert!(budget.contains(&[1u8; 32]));
        assert_eq!(budget.total_bytes(), 90);

        // A single set larger than the whole budget evicts itself (and, being
        // the largest holder, only that peer's sets).
        let evicted = budget.insert(greedy, [5u8; 32], 500, true);
        assert!(evicted.contains(&[5u8; 32]));
        assert!(budget.contains(&[1u8; 32]));
        assert!(budget.total_bytes() <= 100);
    }

    #[test]
    fn many_equal_holders_can_push_out_a_larger_one() {
        // The documented limit of the byte budget: it evens out the holders.
        let mut budget = UnclaimedSetBudget::new(4, 4, 100);
        let honest = peer();
        assert!(budget.insert(honest, [0xaa; 32], 30, true).is_empty());
        let mut evicted = Vec::new();
        for i in 0..10u8 {
            evicted.extend(budget.insert(peer(), [i; 32], 10, true));
        }
        assert!(evicted.contains(&[0xaa; 32]));
        assert!(budget.total_bytes() <= 100);
    }

    #[test]
    fn claimed_and_dropped_sets_leave_the_budget() {
        let mut budget = UnclaimedSetBudget::new(2, 2, 1_000);
        let a = peer();
        budget.insert(a, [1u8; 32], 100, true);
        budget.insert(a, [2u8; 32], 100, false);

        // `claim` reports whether the set had arrived unrequested.
        assert_eq!(budget.claim(&[1u8; 32]), Some(true));
        assert_eq!(budget.claim(&[1u8; 32]), None);
        assert_eq!(budget.total_bytes(), 100);

        // The claimed set no longer counts toward the per-peer limit.
        assert!(budget.insert(a, [3u8; 32], 100, true).is_empty());
        assert_eq!(budget.claim(&[2u8; 32]), Some(false));

        budget.retain(|hash| *hash != [3u8; 32]);
        assert!(!budget.contains(&[3u8; 32]));
        assert!(budget.is_empty());
        assert_eq!(budget.total_bytes(), 0);
    }

    #[test]
    fn the_same_set_is_counted_once() {
        let mut budget = UnclaimedSetBudget::new(2, 2, 1_000);
        let (a, b) = (peer(), peer());
        assert!(budget.insert(a, [1u8; 32], 100, true).is_empty());
        assert!(budget.insert(b, [1u8; 32], 100, true).is_empty());
        assert_eq!(budget.len(), 1);
        assert_eq!(budget.total_bytes(), 100);
    }
}
