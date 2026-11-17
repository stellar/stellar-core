//! Decides where this node's transactions go.
//!
//! Core knows the leader schedule and tells the overlay which validators get
//! transactions now (the "targets", usually the leaders of the current and
//! the next round), or that there is no such list ("everyone": the INV/GETDATA
//! flood to all peers). The router keeps the transactions this node accepted
//! from its own Core until they are externalized, removed or expire, and
//! works out what has to be sent when one is added, when the targets change
//! and when a peer proves which validator it is.
//!
//! Transactions go directly (the body, without an advert) only when every
//! target is this node itself or a connected peer that identified itself.
//! Otherwise the node floods them as before, so that they reach every mempool
//! whatever the topology.
//!
//! Pure bookkeeping: no I/O, no clock of its own. The owner holds it in a
//! mutex that is never held across an await.

use std::collections::{HashMap, VecDeque};
use std::hash::Hash;
use std::sync::Arc;
use std::time::{Duration, Instant};

use super::mempool::TxHash;
use crate::wire::ValidatedTx;

/// A validator's public key (the NodeID's ed25519 key).
pub type NodeId = [u8; 32];

/// At most this many accepted transactions are kept for sending again; the
/// oldest is dropped first. Same bound as the mempool.
pub const MAX_LOCAL_TXS: usize = 100_000;

/// An accepted transaction is given up after this long. Same age as the
/// mempool's.
pub const LOCAL_TX_MAX_AGE: Duration = Duration::from_secs(300);

/// A transaction is not sent again to a peer that got it less than this long
/// ago on the same connection: a target that stays or soon is a target again
/// still has it. Sending again after that covers a body the peer dropped
/// under load (it has no way to ask for it), at a bounded cost while a
/// backlog keeps many transactions pending.
pub const RESEND_INTERVAL: Duration = Duration::from_secs(60);

/// When the targets change, a kept transaction that already went to its
/// targets is sent to a validator that newly became one only if it may still
/// be wanted there:
///  * it is younger than [`FOLLOW_TARGETS_MAX_AGE`], so it may have missed
///    the set the previous targets were building when it reached them, or
///  * [`FOLLOW_TARGETS_AFTER_LEDGERS`] ledgers externalized here since it
///    was accepted and none of them had it. Three, counted on this node: the
///    first may have been built before the transaction was accepted, the
///    second before it reached the leader (this node may see ledgers late,
///    and the transaction takes a network hop), the third was not. Or
///  * it is older than [`FOLLOW_TARGETS_ANYWAY_AFTER`]: no ledger closes
///    (rounds are timing out), and the leaders of later rounds need it.
///
/// Anything else is most likely in the set that is in consensus right now.
/// Sending it on would put a copy into the new target's mempool that, on a
/// node that is running late, arrives after that set externalized there:
/// the new target then holds a finalized transaction and offers it again.
///
/// The age bound has to stay well below the time from a leader's build to
/// the externalize of its set (about 0.5 s at 800 ms ledgers): a fresh
/// transaction that did make the set must reach the new target before that.
pub const FOLLOW_TARGETS_MAX_AGE: Duration = Duration::from_millis(200);
pub const FOLLOW_TARGETS_AFTER_LEDGERS: u64 = 3;
pub const FOLLOW_TARGETS_ANYWAY_AFTER: Duration = Duration::from_secs(3);

/// Hashes of recently externalized transactions that are remembered, to
/// count copies that arrive after their transaction is final (see
/// [`TxRouter::was_finalized`]): about six ledgers of 10,000.
pub const MAX_FINALIZED_TXS: usize = 60_000;

/// Bodies remembered as "received directly from a peer" (see
/// [`TxRouter::note_direct`]). A target receives every node's transactions,
/// so this is about 20 s of a 5,000 TPS load; they are dropped earlier when
/// they are externalized.
pub const MAX_DIRECT_TXS: usize = 100_000;

/// What to send, as decided by the router. The caller does the I/O.
#[derive(Debug)]
pub struct SendPlan<P> {
    /// Bodies to send straight to a peer.
    pub direct: Vec<(P, Vec<Arc<ValidatedTx>>)>,
    /// Transactions to advertise to every peer (the INV/GETDATA flood).
    pub flood: Vec<Arc<ValidatedTx>>,
}

impl<P> Default for SendPlan<P> {
    fn default() -> Self {
        Self {
            direct: Vec::new(),
            flood: Vec::new(),
        }
    }
}

impl<P> SendPlan<P> {
    pub fn is_empty(&self) -> bool {
        self.direct.is_empty() && self.flood.is_empty()
    }
}

/// A transaction that arrived as a body nobody here asked for.
struct DirectTx {
    tx: Arc<ValidatedTx>,
    /// Advertised to this node's peers since (it joined a flood).
    relayed: bool,
}

struct LocalTx<P> {
    tx: Arc<ValidatedTx>,
    added: Instant,
    /// The router's ledger count when it was accepted.
    added_at_ledger: u64,
    /// A plan has looked at it under a direct route: from then on it only
    /// follows the targets under the rule of [`FOLLOW_TARGETS_MAX_AGE`].
    planned: bool,
    /// A send was lost (failed, or the peer went away): the next plan sends
    /// to every target that has no record of it, whatever its age.
    redeliver: bool,
    /// Advertised to every peer: it is in every mempool that wanted it, so
    /// nothing is left to send.
    flooded: bool,
    /// Peers the body was sent to, and when.
    sent: Vec<(P, Instant)>,
    /// Set when a plan found nothing more to send for this transaction: the
    /// route generation it was planned under, and when the first of those
    /// sends is old enough to be repeated. Until then, and while the route
    /// has not changed, a plan skips it without looking at the peers.
    settled: Option<(u64, Instant)>,
}

/// Where the transactions go right now.
#[derive(Debug, PartialEq, Eq)]
enum Route<P> {
    /// Advertise to every peer.
    Flood,
    /// Send the body to these peers only (none if this node is the only
    /// target).
    Direct(Vec<P>),
}

pub struct TxRouter<P> {
    /// This node's own validator key, once Core has sent it.
    local_node: Option<NodeId>,
    /// Validators that get transactions now; `None` = every peer.
    targets: Option<Vec<NodeId>>,
    /// Connected peers that proved which validator they are.
    node_of_peer: HashMap<P, NodeId>,
    /// Transactions this node accepted from its own Core.
    local: HashMap<TxHash, LocalTx<P>>,
    /// The same transactions, oldest first (stale entries are skipped).
    order: VecDeque<(Instant, TxHash)>,
    max_local: usize,
    /// Changes whenever the targets or the identified peers do.
    generation: u64,
    /// Ledgers externalized since the router was created (as told by Core).
    ledgers: u64,
    /// Transactions peers sent here directly. They were not relayed (the
    /// sender delivers to every target itself); they are kept so that this
    /// node can join in, and serve the body, if the sender later has to
    /// flood one.
    direct: lru::LruCache<TxHash, DirectTx>,
    /// Transactions of the last few externalized ledgers.
    finalized: lru::LruCache<TxHash, ()>,
}

impl<P: Copy + Eq + Hash> Default for TxRouter<P> {
    fn default() -> Self {
        Self::new()
    }
}

impl<P: Copy + Eq + Hash> TxRouter<P> {
    pub fn new() -> Self {
        Self::with_capacity(MAX_LOCAL_TXS)
    }

    pub fn with_capacity(max_local: usize) -> Self {
        Self {
            local_node: None,
            targets: None,
            node_of_peer: HashMap::new(),
            local: HashMap::new(),
            order: VecDeque::new(),
            max_local,
            generation: 0,
            ledgers: 0,
            direct: lru::LruCache::new(std::num::NonZeroUsize::new(MAX_DIRECT_TXS).unwrap()),
            finalized: lru::LruCache::new(
                std::num::NonZeroUsize::new(MAX_FINALIZED_TXS).unwrap(),
            ),
        }
    }

    pub fn set_local_node(&mut self, node: NodeId) {
        self.local_node = Some(node);
    }

    /// Replace the targets (`None` = every peer). Returns whether they
    /// changed; the caller then asks for [`Self::plan_all`].
    pub fn set_targets(&mut self, targets: Option<Vec<NodeId>>) -> bool {
        if self.targets == targets {
            return false;
        }
        self.targets = targets;
        self.generation += 1;
        true
    }

    /// A connected peer proved that it is `node`.
    pub fn peer_identified(&mut self, peer: P, node: NodeId) {
        if self.node_of_peer.insert(peer, node) != Some(node) {
            self.generation += 1;
        }
    }

    pub fn identified_peer_count(&self) -> usize {
        self.node_of_peer.len()
    }

    /// The validator a connected peer proved to be.
    pub fn node_of(&self, peer: &P) -> Option<&NodeId> {
        self.node_of_peer.get(peer)
    }

    /// The connected peer of every validator that proved who it is. (If a
    /// validator has two such peers, one of them.)
    pub fn identified_peers(&self) -> HashMap<NodeId, P> {
        self.node_of_peer
            .iter()
            .map(|(peer, node)| (*node, *peer))
            .collect()
    }

    /// The peer is gone. Whatever was sent to it may not have arrived, and a
    /// restarted node has an empty mempool: forget what it was sent.
    pub fn peer_disconnected(&mut self, peer: &P) {
        self.node_of_peer.remove(peer);
        self.generation += 1;
        for entry in self.local.values_mut() {
            let before = entry.sent.len();
            entry.sent.retain(|(p, _)| p != peer);
            if entry.sent.len() != before {
                entry.redeliver = true;
                entry.settled = None;
            }
        }
    }

    /// A send to `peer` failed: these transactions were not delivered.
    pub fn mark_unsent(&mut self, peer: &P, hashes: &[TxHash]) {
        for hash in hashes {
            if let Some(entry) = self.local.get_mut(hash) {
                entry.settled = None;
                entry.redeliver = true;
                entry.sent.retain(|(p, _)| p != peer);
            }
        }
    }

    /// Core accepted `tx` from a client. Keeps it and returns what to send
    /// for it now. A transaction that is already kept is not sent again.
    pub fn add_local(&mut self, tx: Arc<ValidatedTx>, now: Instant) -> SendPlan<P> {
        let hash = *tx.hash();
        if self.local.contains_key(&hash) {
            return SendPlan::default();
        }
        self.evict_expired(now);
        while self.local.len() >= self.max_local {
            if !self.evict_oldest() {
                break;
            }
        }
        self.order.push_back((now, hash));
        let mut entry = LocalTx {
            tx,
            added: now,
            added_at_ledger: self.ledgers,
            planned: false,
            redeliver: false,
            flooded: false,
            sent: Vec::new(),
            settled: None,
        };
        let route = self.route();
        let mut plan = SendPlan::default();
        Self::plan_one(
            &route,
            self.generation,
            self.ledgers,
            &mut entry,
            now,
            &mut plan,
        );
        self.local.insert(hash, entry);
        plan
    }

    /// A peer sent `tx` directly (a body this node did not demand).
    pub fn note_direct(&mut self, tx: Arc<ValidatedTx>) {
        self.direct.put(*tx.hash(), DirectTx { tx, relayed: false });
    }

    /// A peer advertises `hash`. If this node got that transaction directly
    /// and has not relayed it, its origin has fallen back to flooding:
    /// returns the transaction to advertise to the other peers, once.
    pub fn join_flood(&mut self, hash: &TxHash) -> Option<Arc<ValidatedTx>> {
        let entry = self.direct.peek_mut(hash)?;
        if entry.relayed {
            return None;
        }
        entry.relayed = true;
        Some(Arc::clone(&entry.tx))
    }

    /// The body of a transaction this node accepted or received directly,
    /// for a peer that demands it after it left the short-lived buffer of
    /// flooded transactions.
    pub fn body(&self, hash: &TxHash) -> Option<Arc<ValidatedTx>> {
        if let Some(entry) = self.local.get(hash) {
            return Some(Arc::clone(&entry.tx));
        }
        self.direct.peek(hash).map(|entry| Arc::clone(&entry.tx))
    }

    /// Whether any transaction received directly is being kept.
    pub fn has_direct(&self) -> bool {
        !self.direct.is_empty()
    }

    /// These transactions could not be advertised (no peer was connected):
    /// they are flooded again at the next plan.
    pub fn mark_unflooded(&mut self, hashes: &[TxHash]) {
        for hash in hashes {
            if let Some(entry) = self.local.get_mut(hash) {
                entry.settled = None;
                entry.flooded = false;
            }
        }
    }

    /// Core dropped these transactions (or they are externalized: see
    /// [`Self::ledger_externalized`]).
    pub fn remove(&mut self, hashes: &[TxHash]) {
        for hash in hashes {
            self.local.remove(hash);
            self.direct.pop(hash);
        }
        // `order` is cleaned lazily: an entry whose transaction is gone, or
        // was added again later, is skipped when it reaches the front.
    }

    /// A ledger externalized with these transactions. The caller then asks
    /// for [`Self::plan_all`]: transactions that were held back from new
    /// targets may be due now.
    pub fn ledger_externalized(&mut self, hashes: &[TxHash]) {
        self.remove(hashes);
        for hash in hashes {
            self.finalized.put(*hash, ());
        }
        self.ledgers += 1;
    }

    /// Whether `hash` was in one of the last few externalized ledgers: a
    /// body that arrives now is a late copy. (This node's mempool does not
    /// know: it would keep the copy and offer it to the next set it builds,
    /// where validation against the ledger rejects it.)
    pub fn was_finalized(&self, hash: &TxHash) -> bool {
        self.finalized.contains(hash)
    }

    /// What to send for every kept transaction, after the targets or the
    /// identified peers changed, or a ledger externalized.
    pub fn plan_all(&mut self, now: Instant) -> SendPlan<P> {
        self.evict_expired(now);
        let route = self.route();
        let mut plan = SendPlan::default();
        // Oldest first, so that a target receives them in arrival order.
        let hashes: Vec<TxHash> = self
            .order
            .iter()
            .filter(|(added, hash)| self.local.get(hash).is_some_and(|e| e.added == *added))
            .map(|(_, hash)| *hash)
            .collect();
        let generation = self.generation;
        let ledgers = self.ledgers;
        for hash in hashes {
            if let Some(entry) = self.local.get_mut(&hash) {
                Self::plan_one(&route, generation, ledgers, entry, now, &mut plan);
            }
        }
        plan
    }

    pub fn len(&self) -> usize {
        self.local.len()
    }

    pub fn is_empty(&self) -> bool {
        self.local.is_empty()
    }

    pub fn contains(&self, hash: &TxHash) -> bool {
        self.local.contains_key(hash)
    }

    /// Whether transactions currently go directly to targets.
    pub fn is_direct(&self) -> bool {
        matches!(self.route(), Route::Direct(_))
    }

    fn route(&self) -> Route<P> {
        let Some(targets) = &self.targets else {
            return Route::Flood;
        };
        let mut peers = Vec::new();
        for target in targets {
            if self.local_node.as_ref() == Some(target) {
                continue;
            }
            let before = peers.len();
            for (peer, node) in &self.node_of_peer {
                if node == target && !peers.contains(peer) {
                    peers.push(*peer);
                }
            }
            if peers.len() == before {
                // A target this node cannot reach directly: flood, so that
                // the transaction gets there through other nodes.
                return Route::Flood;
            }
        }
        Route::Direct(peers)
    }

    fn plan_one(
        route: &Route<P>,
        generation: u64,
        ledgers: u64,
        entry: &mut LocalTx<P>,
        now: Instant,
        plan: &mut SendPlan<P>,
    ) {
        if entry.flooded {
            return;
        }
        if let Some((planned, due)) = entry.settled {
            if planned == generation && now < due {
                return;
            }
        }
        match route {
            Route::Flood => {
                entry.flooded = true;
                entry.sent.clear();
                plan.flood.push(Arc::clone(&entry.tx));
            }
            Route::Direct(peers) => {
                // See FOLLOW_TARGETS_MAX_AGE.
                let follows = !entry.planned
                    || entry.redeliver
                    || now.saturating_duration_since(entry.added) < FOLLOW_TARGETS_MAX_AGE
                    || now.saturating_duration_since(entry.added) >= FOLLOW_TARGETS_ANYWAY_AFTER
                    || ledgers.saturating_sub(entry.added_at_ledger)
                        >= FOLLOW_TARGETS_AFTER_LEDGERS;
                let mut held_back = false;
                let mut next_due = now + RESEND_INTERVAL;
                for peer in peers {
                    let sent_at = entry.sent.iter().find(|(p, _)| p == peer).map(|(_, at)| *at);
                    match sent_at {
                        Some(at) if now.saturating_duration_since(at) < RESEND_INTERVAL => {
                            next_due = next_due.min(at + RESEND_INTERVAL);
                            continue;
                        }
                        // Sent long ago: again (see RESEND_INTERVAL).
                        Some(_) => {}
                        None if !follows => {
                            held_back = true;
                            continue;
                        }
                        None => {}
                    }
                    entry.sent.retain(|(p, _)| p != peer);
                    entry.sent.push((*peer, now));
                    match plan.direct.iter_mut().find(|(p, _)| p == peer) {
                        Some((_, txs)) => txs.push(Arc::clone(&entry.tx)),
                        None => plan.direct.push((*peer, vec![Arc::clone(&entry.tx)])),
                    }
                }
                entry.planned = true;
                entry.redeliver = false;
                // A transaction that is held back is looked at again by
                // every plan until it went out or is gone.
                entry.settled = if held_back {
                    None
                } else {
                    Some((generation, next_due))
                };
            }
        }
    }

    fn evict_expired(&mut self, now: Instant) {
        while let Some((added, hash)) = self.order.front().copied() {
            let live = self.local.get(&hash).is_some_and(|e| e.added == added);
            if live && now.saturating_duration_since(added) <= LOCAL_TX_MAX_AGE {
                break;
            }
            self.order.pop_front();
            if live {
                self.local.remove(&hash);
            }
        }
    }

    /// Drops the oldest kept transaction. Returns false if there is none.
    fn evict_oldest(&mut self) -> bool {
        while let Some((added, hash)) = self.order.pop_front() {
            if self.local.get(&hash).is_some_and(|e| e.added == added) {
                self.local.remove(&hash);
                return true;
            }
        }
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xdr::tests::valid_transaction_xdr;

    type Router = TxRouter<u32>;

    const ME: NodeId = [1; 32];
    const LEADER1: NodeId = [2; 32];
    const LEADER2: NodeId = [3; 32];
    const OTHER: NodeId = [4; 32];
    const P1: u32 = 10;
    const P2: u32 = 20;
    const P3: u32 = 30;

    fn tx(seq: i64) -> Arc<ValidatedTx> {
        ValidatedTx::from_core_trusted(valid_transaction_xdr(100, seq, 1), 100, 1).unwrap()
    }

    /// A router that knows itself and three identified peers.
    fn mesh() -> Router {
        let mut router = Router::new();
        router.set_local_node(ME);
        router.peer_identified(P1, LEADER1);
        router.peer_identified(P2, LEADER2);
        router.peer_identified(P3, OTHER);
        router
    }

    fn direct_hashes(plan: &SendPlan<u32>, peer: u32) -> Vec<TxHash> {
        plan.direct
            .iter()
            .filter(|(p, _)| *p == peer)
            .flat_map(|(_, txs)| txs.iter().map(|t| *t.hash()))
            .collect()
    }

    #[test]
    fn without_targets_a_transaction_is_flooded_once() {
        let mut router = mesh();
        let now = Instant::now();
        let a = tx(1);
        let plan = router.add_local(a.clone(), now);
        assert!(plan.direct.is_empty());
        assert_eq!(plan.flood.len(), 1);
        assert_eq!(plan.flood[0].hash(), a.hash());
        // Flooded: every mempool has it, nothing is ever sent again.
        assert!(router.plan_all(now).is_empty());
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        assert!(router.plan_all(now).is_empty());
        // Added again: still kept, nothing to send.
        assert!(router.add_local(a, now).is_empty());
        assert_eq!(router.len(), 1);
    }

    #[test]
    fn with_targets_the_body_goes_to_the_targets_only() {
        let mut router = mesh();
        assert!(router.set_targets(Some(vec![LEADER1, LEADER2])));
        assert!(router.is_direct());
        let now = Instant::now();
        let a = tx(1);
        let plan = router.add_local(a.clone(), now);
        assert!(plan.flood.is_empty());
        assert_eq!(direct_hashes(&plan, P1), vec![*a.hash()]);
        assert_eq!(direct_hashes(&plan, P2), vec![*a.hash()]);
        assert!(direct_hashes(&plan, P3).is_empty());
        // Same targets: not sent again.
        assert!(!router.set_targets(Some(vec![LEADER1, LEADER2])));
        assert!(router.plan_all(now).is_empty());
    }

    #[test]
    fn a_pending_transaction_follows_the_targets() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        let now = Instant::now();
        let a = tx(1);
        let b = tx(2);
        router.add_local(a.clone(), now);
        router.add_local(b.clone(), now);
        // `a` is externalized; `b` was not included, and not in the two
        // ledgers after it either.
        router.ledger_externalized(&[*a.hash()]);
        assert!(router.was_finalized(a.hash()));
        assert!(!router.was_finalized(b.hash()));
        router.ledger_externalized(&[]);
        router.ledger_externalized(&[]);
        assert!(router.set_targets(Some(vec![OTHER, LEADER1])));
        let plan = router.plan_all(now + Duration::from_secs(1));
        // The new target gets `b`; the one that stays a target has it.
        assert_eq!(direct_hashes(&plan, P3), vec![*b.hash()]);
        assert!(direct_hashes(&plan, P1).is_empty());
        assert!(direct_hashes(&plan, P2).is_empty());
        assert!(plan.flood.is_empty());
        // Back to an earlier target long after: it is sent again.
        router.set_targets(Some(vec![LEADER2]));
        let later = now + RESEND_INTERVAL + Duration::from_secs(1);
        let plan = router.plan_all(later);
        assert_eq!(direct_hashes(&plan, P2), vec![*b.hash()]);
    }

    #[test]
    fn this_node_as_a_target_needs_no_send() {
        let mut router = mesh();
        router.set_targets(Some(vec![ME, LEADER2]));
        let now = Instant::now();
        let plan = router.add_local(tx(1), now);
        assert_eq!(plan.direct.len(), 1);
        assert_eq!(plan.direct[0].0, P2);
        // The only target: kept, nothing to send, and not flooded.
        router.set_targets(Some(vec![ME]));
        assert!(router.is_direct());
        let plan = router.add_local(tx(2), now);
        assert!(plan.is_empty());
        assert_eq!(router.len(), 2);
    }

    #[test]
    fn an_unreachable_target_means_flooding() {
        let mut router = Router::new();
        router.set_local_node(ME);
        router.peer_identified(P1, LEADER1);
        // LEADER2 is not a connected, identified peer.
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        assert!(!router.is_direct());
        let now = Instant::now();
        let a = tx(1);
        let plan = router.add_local(a.clone(), now);
        assert!(plan.direct.is_empty());
        assert_eq!(plan.flood.len(), 1);
    }

    #[test]
    fn a_transaction_sent_directly_is_flooded_when_a_target_becomes_unreachable() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        let now = Instant::now();
        let a = tx(1);
        assert_eq!(router.add_local(a.clone(), now).direct.len(), 2);
        // The next targets include a node that never identified itself.
        router.set_targets(Some(vec![[9; 32], LEADER1]));
        let plan = router.plan_all(now);
        assert!(plan.direct.is_empty());
        assert_eq!(plan.flood.len(), 1);
        assert_eq!(plan.flood[0].hash(), a.hash());
        // Once flooded it is done.
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        assert!(router.plan_all(now).is_empty());
    }

    #[test]
    fn identification_completes_the_route_and_sends_what_is_pending() {
        let mut router = Router::new();
        router.set_local_node(ME);
        router.set_targets(Some(vec![LEADER1]));
        assert!(!router.is_direct());
        router.peer_identified(P1, LEADER1);
        assert!(router.is_direct());
        let now = Instant::now();
        let a = tx(1);
        assert_eq!(direct_hashes(&router.add_local(a.clone(), now), P1), vec![*a.hash()]);
    }

    #[test]
    fn a_transaction_follows_new_targets_only_while_fresh_or_once_it_missed_three_ledgers() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        let start = Instant::now();
        let old = tx(1);
        assert_eq!(router.add_local(old.clone(), start).direct.len(), 2);

        // The targets change while `old` is most likely in the set its
        // targets are voting on; `fresh` was accepted just before the change
        // and may have missed that set.
        let change = start + Duration::from_millis(700);
        let fresh = tx(2);
        router.add_local(fresh.clone(), change - Duration::from_millis(100));
        let stale = tx(3);
        router.add_local(
            stale.clone(),
            change - FOLLOW_TARGETS_MAX_AGE - Duration::from_millis(1),
        );
        assert!(router.set_targets(Some(vec![LEADER1, OTHER])));
        let plan = router.plan_all(change);
        assert_eq!(direct_hashes(&plan, P3), vec![*fresh.hash()]);
        assert!(direct_hashes(&plan, P1).is_empty());
        // Asking again changes nothing: `old` is still held back.
        assert!(router.plan_all(change + Duration::from_millis(50)).is_empty());

        // The ledger that was in consensus externalizes without `old`: its
        // set may have been built before `old` was accepted. The next one
        // may have been built before `old` reached the leader.
        router.ledger_externalized(&[]);
        assert!(router.plan_all(change + Duration::from_millis(400)).is_empty());
        router.ledger_externalized(&[]);
        assert!(router.plan_all(change + Duration::from_millis(1200)).is_empty());
        // The third was built with `old` in the mempool and did not have it
        // either: now it goes to the new target, once, and so does the one
        // that was just too old at the change.
        router.ledger_externalized(&[]);
        let plan = router.plan_all(change + Duration::from_millis(2000));
        assert_eq!(direct_hashes(&plan, P3), vec![*old.hash(), *stale.hash()]);
        assert!(direct_hashes(&plan, P1).is_empty());
        assert!(router.plan_all(change + Duration::from_millis(2100)).is_empty());
    }

    #[test]
    fn a_held_back_transaction_follows_the_targets_when_no_ledger_closes() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        let start = Instant::now();
        let a = tx(1);
        router.add_local(a.clone(), start);
        // Two rounds time out: the leader of the third round is new.
        assert!(router.set_targets(Some(vec![LEADER2, OTHER])));
        assert!(router.plan_all(start + Duration::from_secs(2)).is_empty());
        let plan = router.plan_all(start + FOLLOW_TARGETS_ANYWAY_AFTER);
        assert_eq!(direct_hashes(&plan, P3), vec![*a.hash()]);
        assert!(direct_hashes(&plan, P2).is_empty());
    }

    #[test]
    fn a_removal_by_core_is_not_a_ledger() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1]));
        let start = Instant::now();
        let a = tx(1);
        let dropped = tx(2);
        router.add_local(a.clone(), start);
        router.add_local(dropped.clone(), start);
        router.set_targets(Some(vec![LEADER1, LEADER2]));
        // Core drops transactions for its own reasons, any number of times:
        // that neither counts as a ledger nor marks them final.
        for _ in 0..5 {
            router.remove(&[*dropped.hash()]);
        }
        assert!(!router.was_finalized(dropped.hash()));
        assert!(router.plan_all(start + Duration::from_secs(1)).is_empty());
    }

    #[test]
    fn a_transaction_that_externalizes_is_never_sent_to_the_new_targets() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1]));
        let start = Instant::now();
        let a = tx(1);
        router.add_local(a.clone(), start);
        // The node closes its ledger late: the targets change 600 ms after
        // `a` went to the leader, which has it in the set in consensus.
        assert!(router.set_targets(Some(vec![LEADER1, LEADER2])));
        assert!(router.plan_all(start + Duration::from_millis(600)).is_empty());
        // That set externalizes two ledgers later (this node sees ledgers
        // late): `a` is gone, and LEADER2 never got a copy that could have
        // arrived after it.
        router.ledger_externalized(&[]);
        assert!(router.plan_all(start + Duration::from_millis(900)).is_empty());
        router.ledger_externalized(&[]);
        assert!(router.plan_all(start + Duration::from_millis(1700)).is_empty());
        router.ledger_externalized(&[*a.hash()]);
        assert!(router.plan_all(start + Duration::from_secs(2)).is_empty());
        assert!(router.was_finalized(a.hash()));
    }

    #[test]
    fn a_transaction_this_node_led_follows_the_targets_under_the_same_rule() {
        let mut router = mesh();
        // This node is the only target: nothing to send.
        router.set_targets(Some(vec![ME]));
        let start = Instant::now();
        let a = tx(1);
        assert!(router.add_local(a.clone(), start).is_empty());
        // Its term ends while `a` may be in its own set in consensus.
        router.set_targets(Some(vec![LEADER1]));
        assert!(router.plan_all(start + Duration::from_millis(600)).is_empty());
        router.ledger_externalized(&[]);
        router.ledger_externalized(&[]);
        router.ledger_externalized(&[]);
        let plan = router.plan_all(start + Duration::from_secs(2));
        assert_eq!(direct_hashes(&plan, P1), vec![*a.hash()]);
    }

    #[test]
    fn a_reconnected_target_gets_the_pending_transactions_again() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1]));
        let now = Instant::now();
        let a = tx(1);
        assert_eq!(router.add_local(a.clone(), now).direct.len(), 1);
        router.set_targets(Some(vec![LEADER2]));
        router.plan_all(now);
        // LEADER1 restarts while it is not a target, then is one again,
        // within the interval in which nothing is sent twice.
        router.peer_disconnected(&P1);
        assert!(router.node_of(&P1).is_none());
        router.peer_identified(P1, LEADER1);
        router.set_targets(Some(vec![LEADER1]));
        let plan = router.plan_all(now + Duration::from_secs(1));
        assert_eq!(direct_hashes(&plan, P1), vec![*a.hash()]);
    }

    #[test]
    fn a_failed_send_is_retried_at_the_next_plan() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1]));
        let now = Instant::now();
        let a = tx(1);
        router.add_local(a.clone(), now);
        assert!(router.plan_all(now).is_empty());
        router.mark_unsent(&P1, &[*a.hash()]);
        assert_eq!(direct_hashes(&router.plan_all(now), P1), vec![*a.hash()]);
    }

    #[test]
    fn two_peers_of_one_validator_both_get_the_body() {
        let mut router = mesh();
        router.peer_identified(P3, LEADER1);
        router.set_targets(Some(vec![LEADER1]));
        let plan = router.add_local(tx(1), Instant::now());
        let mut peers: Vec<u32> = plan.direct.iter().map(|(p, _)| *p).collect();
        peers.sort();
        assert_eq!(peers, vec![P1, P3]);
    }

    #[test]
    fn kept_transactions_are_bounded_and_expire() {
        let mut router = Router::with_capacity(2);
        router.set_local_node(ME);
        router.peer_identified(P1, LEADER1);
        router.set_targets(Some(vec![OTHER])); // unreachable: no sends recorded
        let now = Instant::now();
        let (a, b, c) = (tx(1), tx(2), tx(3));
        router.add_local(a.clone(), now);
        router.add_local(b.clone(), now + Duration::from_secs(1));
        router.add_local(c.clone(), now + Duration::from_secs(2));
        // The oldest was dropped for the third.
        assert_eq!(router.len(), 2);
        assert!(!router.contains(a.hash()));
        assert!(router.contains(b.hash()) && router.contains(c.hash()));
        // Age: `b` expires first.
        router.plan_all(now + Duration::from_secs(1) + LOCAL_TX_MAX_AGE + Duration::from_millis(1));
        assert!(!router.contains(b.hash()));
        assert!(router.contains(c.hash()));
        router.plan_all(now + Duration::from_secs(2) + LOCAL_TX_MAX_AGE + Duration::from_millis(1));
        assert!(router.is_empty());
    }

    #[test]
    fn a_directly_received_transaction_joins_a_flood_once_and_can_be_served() {
        let mut router = mesh();
        let (a, b) = (tx(1), tx(2));
        assert!(!router.has_direct());
        router.note_direct(a.clone());
        assert!(router.has_direct());
        // Not advertised by anyone: nothing to do. Kept, so a demand for it
        // can be answered.
        assert!(router.join_flood(b.hash()).is_none());
        assert_eq!(router.body(a.hash()).unwrap().hash(), a.hash());
        // First advert: join the flood. Later adverts: already done.
        assert_eq!(router.join_flood(a.hash()).unwrap().hash(), a.hash());
        assert!(router.join_flood(a.hash()).is_none());
        assert!(router.body(a.hash()).is_some());
        // Externalized: forgotten.
        router.remove(&[*a.hash()]);
        assert!(router.body(a.hash()).is_none());
        assert!(!router.has_direct());
        // A transaction this node accepted itself can be served as well.
        router.add_local(b.clone(), Instant::now());
        assert!(router.body(b.hash()).is_some());
    }

    #[test]
    fn a_plan_with_unchanged_targets_repeats_a_send_only_after_the_interval() {
        // With every validator as a target the list never changes: a body a
        // peer dropped is only recovered by the periodic plan.
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1, LEADER2, OTHER]));
        let now = Instant::now();
        let a = tx(1);
        assert_eq!(router.add_local(a.clone(), now).direct.len(), 3);
        for secs in [1, 10, 59] {
            assert!(router.plan_all(now + Duration::from_secs(secs)).is_empty());
        }
        let plan = router.plan_all(now + RESEND_INTERVAL);
        assert_eq!(plan.direct.len(), 3);
        assert!(router.plan_all(now + RESEND_INTERVAL + Duration::from_secs(1)).is_empty());
        // A failed send is repeated at the next plan, whatever the interval.
        router.mark_unsent(&P2, &[*a.hash()]);
        let plan = router.plan_all(now + RESEND_INTERVAL + Duration::from_secs(2));
        assert_eq!(direct_hashes(&plan, P2), vec![*a.hash()]);
        assert!(direct_hashes(&plan, P1).is_empty());
    }

    #[test]
    fn a_flood_that_reached_nobody_is_repeated() {
        let mut router = Router::new();
        let now = Instant::now();
        let a = tx(1);
        assert_eq!(router.add_local(a.clone(), now).flood.len(), 1);
        assert!(router.plan_all(now).is_empty());
        // No peer was connected when it was to be advertised.
        router.mark_unflooded(&[*a.hash()]);
        assert_eq!(router.plan_all(now).flood.len(), 1);
        assert!(router.plan_all(now).is_empty());
    }

    #[test]
    fn a_removed_transaction_added_again_is_a_new_one() {
        let mut router = mesh();
        router.set_targets(Some(vec![LEADER1]));
        let now = Instant::now();
        let a = tx(1);
        router.add_local(a.clone(), now);
        router.remove(&[*a.hash()]);
        assert!(router.is_empty());
        assert!(router.plan_all(now).is_empty());
        // Submitted again later: sent again, and it expires by its new age.
        let later = now + Duration::from_secs(200);
        assert_eq!(router.add_local(a.clone(), later).direct.len(), 1);
        router.plan_all(now + LOCAL_TX_MAX_AGE + Duration::from_secs(1));
        assert!(router.contains(a.hash()));
        router.plan_all(later + LOCAL_TX_MAX_AGE + Duration::from_secs(1));
        assert!(router.is_empty());
    }
}
