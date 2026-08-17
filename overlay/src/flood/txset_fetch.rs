//! Tx set fetch lifecycle: source tracking, pending fetches, tiered retry.
//!
//! This is the Rust analog of the C++ `ItemFetcher`/`Tracker` pair, ported for
//! the `HAVE_TX_SET` protocol change (stellar-core PR #5379). Peers that might
//! hold a tx set are tracked in two tiers:
//!
//! - **claimants**: peers that explicitly claimed possession via `HAVE_TX_SET`;
//! - **relayers**: peers that relayed an SCP envelope referencing the hash
//!   (which, once the protocol allows empty-tx-set values and parallel
//!   downloads, no longer implies possession).
//!
//! Fetches are retried on a timeout cadence, targeting claimants first, then
//! relayers, then any connected peer. A fetch whose ask times out drops that
//! peer's claim (the analog of the C++ `DONT_HAVE` handling — the QUIC txset
//! protocol has no negative reply, so a timeout is our miss signal).
//!
//! All methods take `now: Instant` explicitly so the logic is fully
//! deterministic under test; the driver passes `Instant::now()`.

use libp2p::PeerId;
use lru::LruCache;
use std::collections::{HashMap, HashSet};
use std::num::NonZeroUsize;
use std::time::{Duration, Instant};

use super::txset::Hash256;

/// How long one ask may go unanswered before we retarget. Mirrors the C++
/// `Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS`.
pub const TXSET_ASK_TIMEOUT: Duration = Duration::from_millis(1500);

/// Claim grace period: with empty-tx-set values possible and no claimant
/// known, defer the first blind ask up to this long waiting for a
/// `HAVE_TX_SET` claim. Mirrors the C++ claim grace.
pub const TXSET_FETCH_GRACE: Duration = Duration::from_millis(1500);

/// Backoff unit applied between candidate-list rebuilds, scaled by the number
/// of rebuilds so far (capped). Mirrors the C++ rebuild backoff.
pub const TXSET_REBUILD_BACKOFF_UNIT: Duration = Duration::from_millis(1500);
pub const TXSET_MAX_REBUILD_BACKOFF_MULT: u32 = 10;

/// Absolute age backstop: a fetch this old is abandoned even if slot-based
/// purging never caught it (leak protection; slot purging is the normal path).
pub const TXSET_FETCH_MAX_AGE: Duration = Duration::from_secs(600);

/// Distinct hashes for which sources (claims/relays) are remembered. Matches
/// the C++ `BUFFERED_CLAIMS_CACHE_SIZE`; also subsumes the old
/// `txset_sources` LRU (same capacity).
pub const TXSET_SOURCES_CACHE_SIZE: usize = 1000;

/// Per-hash bound on remembered peers in each tier.
pub const MAX_SOURCES_PER_HASH: usize = 8;

/// Which tier the selected peer came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AskTier {
    /// The peer explicitly claimed possession via `HAVE_TX_SET`.
    Claimant,
    /// The peer relayed an SCP envelope referencing the hash.
    Relayer,
    /// No better information: any connected peer.
    Blind,
}

/// Outcome of the claim grace period, reported once per fetch on its first
/// dispatched ask (only when the grace was enabled for the fetch).
#[derive(Debug, Clone, Copy)]
pub struct GraceOutcome {
    /// Time from fetch creation to the first ask.
    pub waited: Duration,
    /// Whether the first ask targeted a claimant.
    pub satisfied: bool,
}

/// A dispatch decision: ask `peer` for `hash`. Produced by [`TxSetFetcher::tick`]
/// and [`TxSetFetcher::dispatch_one`]; the driver performs the actual send and
/// reports back via `mark_sent` / `mark_send_failed`.
#[derive(Debug)]
pub struct Ask {
    pub hash: Hash256,
    pub peer: PeerId,
    pub tier: AskTier,
    /// Present only on the fetch's first ask when the grace was enabled.
    pub grace_outcome: Option<GraceOutcome>,
}

/// A completed fetch, for latency metrics.
#[derive(Debug)]
pub struct CompletedFetch {
    pub slot: u32,
    pub elapsed: Duration,
}

#[derive(Default)]
struct TxSetSources {
    claimants: Vec<PeerId>,
    relayers: Vec<PeerId>,
}

fn push_bounded(list: &mut Vec<PeerId>, peer: PeerId) {
    if list.contains(&peer) {
        return;
    }
    if list.len() >= MAX_SOURCES_PER_HASH {
        list.remove(0);
    }
    list.push(peer);
}

struct PendingFetch {
    /// The peer currently asked; `None` while undispatched (grace period,
    /// no candidates, backoff, or send failure).
    peer: Option<PeerId>,
    /// When the current ask was (last) dispatched or confirmed on the wire.
    sent_at: Instant,
    created_at: Instant,
    slot: u32,
    /// Number of candidate-list rebuilds; scales the backoff.
    rebuilds: u32,
    /// Gate for the next dispatch attempt (backoff after a rebuild).
    next_dispatch_at: Instant,
    /// Peers asked since the last rebuild.
    asked: HashSet<PeerId>,
    /// Whether the claim grace applies to this fetch (captured at creation).
    grace_enabled: bool,
    /// Whether the grace outcome has been reported (first ask happened).
    grace_resolved: bool,
}

/// Result of one housekeeping tick.
#[derive(Debug, Default)]
pub struct TickResult {
    /// Asks the driver should send now.
    pub asks: Vec<Ask>,
    /// Fetches abandoned by the absolute age backstop.
    pub expired: Vec<Hash256>,
}

/// Tracks who might have which tx set and drives pending fetches to
/// completion. Pure state machine: no I/O, no clocks of its own.
pub struct TxSetFetcher {
    sources: LruCache<Hash256, TxSetSources>,
    pending: HashMap<Hash256, PendingFetch>,
    /// Whether the current ledger protocol admits empty-tx-set values (the
    /// condition under which SCP relayers may not possess referenced sets and
    /// the claim grace is worth paying). Set from core via IPC.
    empty_tx_sets_possible: bool,
}

impl TxSetFetcher {
    pub fn new() -> Self {
        TxSetFetcher {
            sources: LruCache::new(NonZeroUsize::new(TXSET_SOURCES_CACHE_SIZE).unwrap()),
            pending: HashMap::new(),
            empty_tx_sets_possible: false,
        }
    }

    pub fn set_empty_tx_sets_possible(&mut self, possible: bool) {
        self.empty_tx_sets_possible = possible;
    }

    /// Record that `peer` relayed an SCP envelope referencing `hash` (weak
    /// evidence of possession).
    pub fn record_relayer(&mut self, hash: Hash256, peer: PeerId) {
        push_bounded(
            &mut self
                .sources
                .get_or_insert_mut(hash, Default::default)
                .relayers,
            peer,
        );
    }

    /// Record that `peer` explicitly claimed possession of `hash` via
    /// `HAVE_TX_SET` (strong evidence). Returns true if the claim is relevant
    /// to a pending fetch that currently has no ask outstanding, i.e. the
    /// driver should run a dispatch pass promptly rather than wait for the
    /// next tick.
    pub fn record_claim(&mut self, hash: Hash256, peer: PeerId) -> bool {
        push_bounded(
            &mut self
                .sources
                .get_or_insert_mut(hash, Default::default)
                .claimants,
            peer,
        );
        match self.pending.get_mut(&hash) {
            Some(entry) if entry.peer.is_none() => {
                // Act on the claim immediately (mirrors the C++ tracker
                // canceling its retry timer on a claim): lift any rebuild
                // backoff so the next dispatch pass is not gated.
                entry.next_dispatch_at = entry.created_at;
                true
            }
            _ => false,
        }
    }

    /// Start fetching `hash` for `slot`. Returns true if this created a new
    /// pending fetch; false if one already existed (in which case its slot is
    /// raised to `slot` if that is newer, so slot-based purging can never
    /// strand a fetch that a newer slot still needs).
    pub fn start_fetch(&mut self, hash: Hash256, slot: u32, now: Instant) -> bool {
        if let Some(existing) = self.pending.get_mut(&hash) {
            existing.slot = existing.slot.max(slot);
            return false;
        }
        self.pending.insert(
            hash,
            PendingFetch {
                peer: None,
                sent_at: now,
                created_at: now,
                slot,
                rebuilds: 0,
                next_dispatch_at: now,
                asked: HashSet::new(),
                grace_enabled: self.empty_tx_sets_possible,
                grace_resolved: false,
            },
        );
        true
    }

    /// Whether a fetch for `hash` is pending.
    pub fn is_pending(&self, hash: &Hash256) -> bool {
        self.pending.contains_key(hash)
    }

    pub fn pending_len(&self) -> usize {
        self.pending.len()
    }

    /// The peer currently asked for `hash`, if an ask is outstanding.
    pub fn asked_peer(&self, hash: &Hash256) -> Option<PeerId> {
        self.pending.get(hash).and_then(|e| e.peer)
    }

    /// The fetch completed: the set arrived (from anyone). Returns latency
    /// info if a fetch was pending.
    pub fn complete(&mut self, hash: &Hash256, now: Instant) -> Option<CompletedFetch> {
        self.pending.remove(hash).map(|entry| CompletedFetch {
            slot: entry.slot,
            elapsed: now.saturating_duration_since(entry.created_at),
        })
    }

    /// The send for `hash`'s current ask actually reached the wire: restart
    /// the response window (excludes local queueing delay from the timeout).
    pub fn mark_sent(&mut self, hash: &Hash256, now: Instant) {
        if let Some(entry) = self.pending.get_mut(hash) {
            if entry.peer.is_some() {
                entry.sent_at = now;
            }
        }
    }

    /// The send for `hash`'s current ask failed: clear the ask so the next
    /// dispatch pass retargets immediately. The failed peer stays in `asked`
    /// for this round so we don't hammer it.
    pub fn mark_send_failed(&mut self, hash: &Hash256) {
        if let Some(entry) = self.pending.get_mut(hash) {
            entry.peer = None;
        }
    }

    /// A peer disconnected: clear any outstanding asks to it (they will be
    /// retargeted on the next dispatch pass) and forget it as a source.
    pub fn peer_disconnected(&mut self, peer: &PeerId) {
        for entry in self.pending.values_mut() {
            if entry.peer.as_ref() == Some(peer) {
                entry.peer = None;
            }
        }
        // LruCache has no retain; walk the (bounded) entries.
        for (_, sources) in self.sources.iter_mut() {
            sources.claimants.retain(|p| p != peer);
            sources.relayers.retain(|p| p != peer);
        }
    }

    /// Drop pending fetches for slots strictly below `slot` (mirrors the tx
    /// set cache eviction horizon). Fetches created before slots were known
    /// (slot 0) are kept.
    pub fn evict_before(&mut self, slot: u32) {
        self.pending
            .retain(|_, entry| entry.slot == 0 || entry.slot >= slot);
    }

    /// Run one housekeeping pass: time out stale asks (dropping the asked
    /// peer's claim — our miss signal), dispatch undispatched fetches, and
    /// abandon fetches past the absolute age backstop.
    pub fn tick(&mut self, now: Instant, connected: &[PeerId]) -> TickResult {
        let mut result = TickResult::default();

        // Phase 1: expire by age, and time out stale asks.
        let mut timed_out: Vec<Hash256> = Vec::new();
        self.pending.retain(|hash, entry| {
            if now.saturating_duration_since(entry.created_at) >= TXSET_FETCH_MAX_AGE {
                result.expired.push(*hash);
                return false;
            }
            if entry.peer.is_some()
                && now.saturating_duration_since(entry.sent_at) >= TXSET_ASK_TIMEOUT
            {
                timed_out.push(*hash);
            }
            true
        });
        for hash in timed_out {
            let entry = self.pending.get_mut(&hash).unwrap();
            let peer = entry.peer.take().unwrap();
            // The ask went unanswered: any claim this peer made was wrong.
            if let Some(sources) = self.sources.peek_mut(&hash) {
                sources.claimants.retain(|p| p != &peer);
            }
        }

        // Phase 2: dispatch whatever is undispatched and eligible.
        let hashes: Vec<Hash256> = self
            .pending
            .iter()
            .filter(|(_, e)| e.peer.is_none())
            .map(|(h, _)| *h)
            .collect();
        for hash in hashes {
            if let Some(ask) = self.try_dispatch(hash, now, connected) {
                result.asks.push(ask);
            }
        }
        result
    }

    /// Attempt to dispatch the fetch for `hash` right now (used at fetch start
    /// and when a claim arrives, so a claim is acted on immediately rather
    /// than at the next tick). No-op if an ask is already outstanding.
    pub fn dispatch_one(
        &mut self,
        hash: Hash256,
        now: Instant,
        connected: &[PeerId],
    ) -> Option<Ask> {
        match self.pending.get(&hash) {
            Some(entry) if entry.peer.is_none() => self.try_dispatch(hash, now, connected),
            _ => None,
        }
    }

    /// Core dispatch: pick a target by tier, honoring grace and backoff.
    /// Precondition: `hash` is pending and has no ask outstanding.
    fn try_dispatch(&mut self, hash: Hash256, now: Instant, connected: &[PeerId]) -> Option<Ask> {
        let entry = self.pending.get_mut(&hash)?;
        if now < entry.next_dispatch_at {
            return None; // rebuild backoff
        }

        let empty_sources = TxSetSources::default();
        let sources = self.sources.peek(&hash).unwrap_or(&empty_sources);

        let pick = |tier: &[PeerId], skip_asked: bool| -> Option<PeerId> {
            tier.iter()
                .find(|p| connected.contains(p) && (!skip_asked || !entry.asked.contains(p)))
                .cloned()
        };

        let selected: Option<(PeerId, AskTier)> =
            // Fresh claimants first, then claimants we already asked (a claim
            // re-enables a peer that previously missed).
            pick(&sources.claimants, true)
                .or_else(|| pick(&sources.claimants, false))
                .map(|p| (p, AskTier::Claimant))
                .or_else(|| pick(&sources.relayers, true).map(|p| (p, AskTier::Relayer)))
                .or_else(|| {
                    connected
                        .iter()
                        .find(|p| !entry.asked.contains(p))
                        .cloned()
                        .map(|p| (p, AskTier::Blind))
                });

        let (peer, tier) = match selected {
            Some(sel) => sel,
            None => {
                // Every known candidate has been asked this round (or nobody
                // is connected): rebuild the round with backoff.
                entry.asked.clear();
                entry.rebuilds += 1;
                let mult = entry.rebuilds.min(TXSET_MAX_REBUILD_BACKOFF_MULT);
                entry.next_dispatch_at = now + TXSET_REBUILD_BACKOFF_UNIT * mult;
                return None;
            }
        };

        // Claim grace: before the first ask, with the grace enabled and no
        // claimant available, hold off a bounded time in case a claim arrives.
        if entry.grace_enabled
            && !entry.grace_resolved
            && tier != AskTier::Claimant
            && now.saturating_duration_since(entry.created_at) < TXSET_FETCH_GRACE
        {
            return None;
        }

        let grace_outcome = if entry.grace_enabled && !entry.grace_resolved {
            entry.grace_resolved = true;
            Some(GraceOutcome {
                waited: now.saturating_duration_since(entry.created_at),
                satisfied: tier == AskTier::Claimant,
            })
        } else {
            entry.grace_resolved = true;
            None
        };

        entry.peer = Some(peer);
        entry.sent_at = now;
        entry.asked.insert(peer);

        Some(Ask {
            hash,
            peer,
            tier,
            grace_outcome,
        })
    }
}

impl Default for TxSetFetcher {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hash(n: u8) -> Hash256 {
        [n; 32]
    }

    fn setup() -> (TxSetFetcher, Instant, PeerId, PeerId, PeerId) {
        (
            TxSetFetcher::new(),
            Instant::now(),
            PeerId::random(),
            PeerId::random(),
            PeerId::random(),
        )
    }

    #[test]
    fn dispatch_prefers_claimant_over_relayer_over_blind() {
        let (mut f, now, claimant, relayer, other) = setup();
        f.record_relayer(hash(1), relayer);
        f.record_claim(hash(1), claimant);

        assert!(f.start_fetch(hash(1), 5, now));
        let connected = vec![other, relayer, claimant];
        let ask = f.dispatch_one(hash(1), now, &connected).unwrap();
        assert_eq!(ask.peer, claimant);
        assert_eq!(ask.tier, AskTier::Claimant);

        // With the claimant gone, the relayer tier is next.
        let mut f2 = TxSetFetcher::new();
        f2.record_relayer(hash(1), relayer);
        f2.start_fetch(hash(1), 5, now);
        let ask = f2.dispatch_one(hash(1), now, &connected).unwrap();
        assert_eq!(ask.peer, relayer);
        assert_eq!(ask.tier, AskTier::Relayer);

        // With no sources at all, any connected peer is asked (blind).
        let mut f3 = TxSetFetcher::new();
        f3.start_fetch(hash(1), 5, now);
        let ask = f3.dispatch_one(hash(1), now, &connected).unwrap();
        assert_eq!(ask.tier, AskTier::Blind);
    }

    #[test]
    fn disconnected_sources_are_skipped() {
        let (mut f, now, claimant, relayer, other) = setup();
        f.record_claim(hash(1), claimant);
        f.record_relayer(hash(1), relayer);
        f.start_fetch(hash(1), 5, now);

        // Neither source is connected: blind ask to the only connected peer.
        let ask = f.dispatch_one(hash(1), now, &[other]).unwrap();
        assert_eq!(ask.peer, other);
        assert_eq!(ask.tier, AskTier::Blind);
    }

    #[test]
    fn timeout_retargets_and_drops_wrong_claim() {
        let (mut f, now, claimant, relayer, _) = setup();
        f.record_claim(hash(1), claimant);
        f.record_relayer(hash(1), relayer);
        f.start_fetch(hash(1), 5, now);

        let connected = vec![claimant, relayer];
        let ask = f.dispatch_one(hash(1), now, &connected).unwrap();
        assert_eq!(ask.peer, claimant);

        // Before the timeout: nothing happens.
        let r = f.tick(now + TXSET_ASK_TIMEOUT / 2, &connected);
        assert!(r.asks.is_empty());

        // At the timeout: the claim was wrong (dropped), retarget to relayer.
        let r = f.tick(now + TXSET_ASK_TIMEOUT, &connected);
        assert_eq!(r.asks.len(), 1);
        assert_eq!(r.asks[0].peer, relayer);
        assert_eq!(r.asks[0].tier, AskTier::Relayer);
    }

    #[test]
    fn claim_reenables_previously_asked_peer() {
        let (mut f, now, p1, p2, _) = setup();
        let connected = vec![p1, p2];
        f.start_fetch(hash(1), 5, now);

        // Blind asks burn through both peers.
        let first = f.dispatch_one(hash(1), now, &connected).unwrap().peer;
        let now2 = now + TXSET_ASK_TIMEOUT;
        let second = f.tick(now2, &connected).asks.pop().unwrap().peer;
        assert_ne!(first, second);

        // Both asked and timed out: the round is exhausted (rebuild+backoff),
        // no ask goes out.
        let now3 = now2 + TXSET_ASK_TIMEOUT;
        assert!(f.tick(now3, &connected).asks.is_empty());

        // The first peer now claims possession: the claim lifts the rebuild
        // backoff, and the peer is re-asked in spite of having been asked
        // (and missed) before.
        assert!(f.record_claim(hash(1), first));
        let asks = f.tick(now3, &connected).asks;
        assert_eq!(asks.len(), 1);
        assert_eq!(asks[0].peer, first);
        assert_eq!(asks[0].tier, AskTier::Claimant);
    }

    #[test]
    fn claim_with_no_pending_fetch_is_passive() {
        let (mut f, now, p1, p2, _) = setup();
        assert!(!f.record_claim(hash(1), p1));
        assert_eq!(f.pending_len(), 0);

        // A claim while an ask is outstanding is recorded but not actionable
        // (mirrors C++: does not interrupt the outstanding ask).
        f.start_fetch(hash(2), 5, now);
        f.dispatch_one(hash(2), now, &[p1]);
        assert!(!f.record_claim(hash(2), p2));
        assert_eq!(f.asked_peer(&hash(2)), Some(p1));
    }

    #[test]
    fn claim_for_undispatched_fetch_lifts_backoff_and_dispatches() {
        let (mut f, now, p1, _, _) = setup();
        f.start_fetch(hash(1), 5, now);
        // Dispatch attempt with nobody connected: rebuild + backoff.
        assert!(f.dispatch_one(hash(1), now, &[]).is_none());

        // Peer connects and claims: actionable, and the backoff no longer
        // gates the dispatch — it happens immediately.
        assert!(f.record_claim(hash(1), p1));
        let ask = f.dispatch_one(hash(1), now, &[p1]).unwrap();
        assert_eq!(ask.peer, p1);
        assert_eq!(ask.tier, AskTier::Claimant);
    }

    #[test]
    fn buffered_claim_seeds_first_ask() {
        // Claim arrives before any fetch: recorded passively, and the first
        // ask targets the claimant once a fetch starts.
        let (mut f, now, claimant, other, _) = setup();
        assert!(!f.record_claim(hash(1), claimant));

        f.start_fetch(hash(1), 5, now);
        let ask = f.dispatch_one(hash(1), now, &[other, claimant]).unwrap();
        assert_eq!(ask.peer, claimant);
        assert_eq!(ask.tier, AskTier::Claimant);
    }

    #[test]
    fn repeated_claims_deduplicate_and_bound() {
        let (mut f, _, p1, p2, _) = setup();
        f.record_claim(hash(1), p1);
        f.record_claim(hash(1), p1);
        f.record_claim(hash(1), p1);
        f.record_claim(hash(1), p2);
        let sources = f.sources.peek(&hash(1)).unwrap();
        assert_eq!(sources.claimants.len(), 2);

        // The per-hash bound holds under a flood of distinct claimants.
        for _ in 0..(MAX_SOURCES_PER_HASH * 2) {
            f.record_claim(hash(1), PeerId::random());
        }
        assert_eq!(
            f.sources.peek(&hash(1)).unwrap().claimants.len(),
            MAX_SOURCES_PER_HASH
        );
    }

    #[test]
    fn send_failure_keeps_fetch_and_retargets() {
        let (mut f, now, p1, p2, _) = setup();
        f.start_fetch(hash(1), 5, now);
        let ask = f.dispatch_one(hash(1), now, &[p1, p2]).unwrap();
        let failed = ask.peer;

        f.mark_send_failed(&hash(1));
        assert!(f.is_pending(&hash(1)));
        assert!(f.asked_peer(&hash(1)).is_none());

        // Retarget goes to the other peer (failed one stays in `asked`).
        let ask = f.dispatch_one(hash(1), now, &[p1, p2]).unwrap();
        assert_ne!(ask.peer, failed);
    }

    #[test]
    fn disconnect_clears_ask_but_keeps_fetch() {
        let (mut f, now, p1, p2, _) = setup();
        f.start_fetch(hash(1), 5, now);
        let ask = f.dispatch_one(hash(1), now, &[p1]).unwrap();
        assert_eq!(ask.peer, p1);

        f.peer_disconnected(&p1);
        assert!(f.is_pending(&hash(1)), "fetch must survive the disconnect");
        assert!(f.asked_peer(&hash(1)).is_none());

        // Next tick retargets to the remaining peer.
        let asks = f.tick(now + Duration::from_millis(1), &[p2]).asks;
        assert_eq!(asks.len(), 1);
        assert_eq!(asks[0].peer, p2);
    }

    #[test]
    fn fetch_with_no_peers_parks_until_a_peer_appears() {
        let (mut f, now, p1, _, _) = setup();
        f.start_fetch(hash(1), 5, now);
        assert!(f.dispatch_one(hash(1), now, &[]).is_none());
        assert!(f.is_pending(&hash(1)), "fetch must not be lost");

        // A peer connects; after the rebuild backoff the fetch dispatches.
        let later = now + TXSET_REBUILD_BACKOFF_UNIT;
        let asks = f.tick(later, &[p1]).asks;
        assert_eq!(asks.len(), 1);
        assert_eq!(asks[0].peer, p1);
    }

    #[test]
    fn rebuild_backoff_scales_with_attempts() {
        let (mut f, now, _, _, _) = setup();
        f.start_fetch(hash(1), 5, now);

        // Two empty dispatch attempts (rebuilds 1 and 2). The second is only
        // permitted once the first backoff (1 * unit) expired.
        assert!(f.dispatch_one(hash(1), now, &[]).is_none());
        let after_first = now + TXSET_REBUILD_BACKOFF_UNIT;
        assert!(f.tick(after_first, &[]).asks.is_empty()); // rebuild #2

        // Backoff is now 2 * unit: a peer connecting at 1 * unit later is not
        // asked yet; at 2 * unit it is.
        let p = PeerId::random();
        let too_soon = after_first + TXSET_REBUILD_BACKOFF_UNIT - Duration::from_millis(1);
        assert!(f.tick(too_soon, &[p]).asks.is_empty());
        let due = after_first + 2 * TXSET_REBUILD_BACKOFF_UNIT;
        assert_eq!(f.tick(due, &[p]).asks.len(), 1);
    }

    #[test]
    fn complete_reports_slot_and_latency() {
        let (mut f, now, p1, _, _) = setup();
        f.start_fetch(hash(1), 42, now);
        f.dispatch_one(hash(1), now, &[p1]);

        let done = f
            .complete(&hash(1), now + Duration::from_millis(300))
            .unwrap();
        assert_eq!(done.slot, 42);
        assert_eq!(done.elapsed, Duration::from_millis(300));
        assert!(!f.is_pending(&hash(1)));
        assert!(f.complete(&hash(1), now).is_none());
    }

    #[test]
    fn duplicate_start_fetch_raises_slot_only() {
        let (mut f, now, _, _, _) = setup();
        assert!(f.start_fetch(hash(1), 5, now));
        assert!(!f.start_fetch(hash(1), 9, now));
        assert!(!f.start_fetch(hash(1), 3, now));
        assert_eq!(f.pending.get(&hash(1)).unwrap().slot, 9);
        assert_eq!(f.pending_len(), 1);
    }

    #[test]
    fn evict_before_purges_old_slots_keeps_new_and_unknown() {
        let (mut f, now, _, _, _) = setup();
        f.start_fetch(hash(1), 10, now);
        f.start_fetch(hash(2), 100, now);
        f.start_fetch(hash(3), 0, now); // slot unknown

        f.evict_before(50);
        assert!(!f.is_pending(&hash(1)));
        assert!(f.is_pending(&hash(2)));
        assert!(f.is_pending(&hash(3)));
    }

    #[test]
    fn age_backstop_expires_ancient_fetches() {
        let (mut f, now, p1, _, _) = setup();
        f.start_fetch(hash(1), 5, now);
        let r = f.tick(now + TXSET_FETCH_MAX_AGE, &[p1]);
        assert_eq!(r.expired, vec![hash(1)]);
        assert!(!f.is_pending(&hash(1)));
    }

    // --- grace period ---

    #[test]
    fn grace_defers_blind_ask_until_claim_or_expiry() {
        let (mut f, now, relayer, claimant, _) = setup();
        f.set_empty_tx_sets_possible(true);
        f.record_relayer(hash(1), relayer);
        f.start_fetch(hash(1), 5, now);

        // No claimant: the first ask is deferred, even with a relayer ready.
        assert!(f.dispatch_one(hash(1), now, &[relayer]).is_none());
        assert!(f
            .tick(now + TXSET_FETCH_GRACE / 2, &[relayer])
            .asks
            .is_empty());

        // A claim preempts the wait immediately.
        assert!(f.record_claim(hash(1), claimant));
        let asks = f
            .tick(now + TXSET_FETCH_GRACE / 2, &[relayer, claimant])
            .asks;
        assert_eq!(asks.len(), 1);
        assert_eq!(asks[0].peer, claimant);
        let outcome = asks[0].grace_outcome.expect("first ask reports grace");
        assert!(outcome.satisfied);
        assert_eq!(outcome.waited, TXSET_FETCH_GRACE / 2);
    }

    #[test]
    fn grace_expiry_falls_back_to_relayer() {
        let (mut f, now, relayer, _, _) = setup();
        f.set_empty_tx_sets_possible(true);
        f.record_relayer(hash(1), relayer);
        f.start_fetch(hash(1), 5, now);

        assert!(f.dispatch_one(hash(1), now, &[relayer]).is_none());
        let asks = f.tick(now + TXSET_FETCH_GRACE, &[relayer]).asks;
        assert_eq!(asks.len(), 1);
        assert_eq!(asks[0].peer, relayer);
        let outcome = asks[0].grace_outcome.expect("first ask reports grace");
        assert!(!outcome.satisfied);
        assert_eq!(outcome.waited, TXSET_FETCH_GRACE);
    }

    #[test]
    fn grace_reported_once_then_never_again() {
        let (mut f, now, p1, p2, _) = setup();
        f.set_empty_tx_sets_possible(true);
        f.start_fetch(hash(1), 5, now);

        let t1 = now + TXSET_FETCH_GRACE;
        let asks = f.tick(t1, &[p1, p2]).asks;
        assert!(asks[0].grace_outcome.is_some());

        // Retarget after timeout: no second grace outcome.
        let t2 = t1 + TXSET_ASK_TIMEOUT;
        let asks = f.tick(t2, &[p1, p2]).asks;
        assert_eq!(asks.len(), 1);
        assert!(asks[0].grace_outcome.is_none());
    }

    #[test]
    fn grace_disabled_means_immediate_first_ask_and_no_outcome() {
        let (mut f, now, relayer, _, _) = setup();
        // empty_tx_sets_possible defaults to false.
        f.record_relayer(hash(1), relayer);
        f.start_fetch(hash(1), 5, now);

        let ask = f.dispatch_one(hash(1), now, &[relayer]).unwrap();
        assert_eq!(ask.peer, relayer);
        assert!(ask.grace_outcome.is_none());
    }

    #[test]
    fn grace_flag_captured_at_fetch_creation() {
        let (mut f, now, p1, _, _) = setup();
        f.start_fetch(hash(1), 5, now);
        // Turning the flag on later must not retroactively delay this fetch.
        f.set_empty_tx_sets_possible(true);
        assert!(f.dispatch_one(hash(1), now, &[p1]).is_some());
    }
}
