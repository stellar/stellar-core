//! Spanning tree for the leader's tx-set push.
//!
//! Without a tree the round's leader writes its set to every peer. With one,
//! it writes it to a few (its children in the tree) and every node that
//! receives it passes the bytes on to its own children. The tree of a slot
//! and leader is a function of things every node already agrees on, as the
//! leader schedule is: the nodes of the transitive quorum and a hash.
//!
//! * [`tree_order`]: the leader first, the other nodes by
//!   `SHA256(seed || leader || node)`.
//! * Position `i` of the order has the children `F*i+1 ..= F*i+F`: the
//!   complete F-ary tree, the least depth a fanout of F allows.
//! * [`TreeHeader`]: what a pushed frame says about itself, in front of the
//!   ordinary tx-set frame.
//! * [`relay_plan`]: whether this node passes a frame on, and to whom.
//! * [`rtt_order`]: an order chosen by the leader from its round-trip times
//!   (it travels in the header, because no other node can recompute it).
//!
//! Nothing here is trusted by the receiver of a set: a set is still verified
//! and filed by the hash of its content. The tree only decides who sends the
//! bytes to whom.

use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::time::Duration;

pub type NodeId = [u8; 32];

/// Stands where an ordinary tx-set frame has the big-endian length of its
/// XDR. No length can take this value (the limit is 16 MiB).
pub const TREE_TAG: u32 = 0xFFFF_FFF1;

/// Stands in the same place in a ping or a pong: a frame of 13 bytes on the
/// tx-set stream by which a node measures the round trip to a peer.
pub const TREE_PING_TAG: u32 = 0xFFFF_FFF2;

/// A round-trip probe: the peer sends back what it was sent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Probe {
    /// Answer with a pong that carries the same number.
    Ping(u64),
    /// The answer to a ping this node sent with that number.
    Pong(u64),
}

impl Probe {
    pub fn encode(self) -> [u8; 13] {
        let (kind, value) = match self {
            Probe::Ping(value) => (1u8, value),
            Probe::Pong(value) => (2u8, value),
        };
        let mut out = [0u8; 13];
        out[..4].copy_from_slice(&TREE_PING_TAG.to_be_bytes());
        out[4] = kind;
        out[5..].copy_from_slice(&value.to_be_bytes());
        out
    }

    /// `Some(None)`: tagged as a probe but malformed.
    pub fn parse(frame: &[u8]) -> Option<Option<Self>> {
        if frame.len() < 4 || frame[..4] != TREE_PING_TAG.to_be_bytes() {
            return None;
        }
        if frame.len() != 13 {
            return Some(None);
        }
        let value = u64::from_be_bytes(frame[5..].try_into().unwrap());
        Some(match frame[4] {
            1 => Some(Probe::Ping(value)),
            2 => Some(Probe::Pong(value)),
            _ => None,
        })
    }
}

/// Bytes of a header without an explicit order.
pub const TREE_HEADER_FIXED: usize = 4 + 1 + 1 + 1 + 1 + 4 + 32 + 8 + 2;

/// Nodes a tree may have (an explicit order is two bytes per node).
pub const MAX_TREE_NODES: usize = 1024;

/// Bytes of the node-list digest behind an explicit order.
pub const TREE_NODES_DIGEST: usize = 8;

/// The largest header a frame can carry.
pub const MAX_TREE_HEADER: usize = TREE_HEADER_FIXED + 2 * MAX_TREE_NODES + TREE_NODES_DIGEST;

/// Frames a node relays for one slot and leader. A leader pushes one set per
/// slot it leads, two if it built one early and another at the trigger.
pub const MAX_RELAYS_PER_SLOT_AND_LEADER: u8 = 2;

/// Relays that may be queued or in flight to one child at a time. A child
/// that stops reading its tx-set stream then pins at most this many frames
/// in the relay's memory; further frames are not relayed to it, and it asks
/// for them instead.
pub const MAX_IN_FLIGHT_RELAYS_PER_CHILD: u32 = 1;

/// How a node pushes the set it leads with, as its Core configures it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TreeMode {
    /// As before the tree: an untagged frame to every peer.
    Off,
    /// To every peer, tagged (the frame says when it was sent), never
    /// relayed. The baseline for measurements.
    Star,
    /// Down the tree derived from the slot's seed.
    Hash,
    /// Down a tree the leader lays out by its round-trip times.
    Rtt,
}

impl TreeMode {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Off),
            1 => Some(Self::Star),
            2 => Some(Self::Hash),
            3 => Some(Self::Rtt),
            _ => None,
        }
    }

    /// Whether nodes in this mode relay tagged frames and wait for them.
    pub fn relays(self) -> bool {
        matches!(self, Self::Hash | Self::Rtt)
    }
}

/// What a tagged frame is, from its header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameKind {
    /// Sent to every peer by the leader; not to be relayed.
    Star = 1,
    /// Travels down the tree every node derives from the slot's seed.
    Hash = 2,
    /// Travels down the tree whose order the header carries.
    Explicit = 3,
}

/// The smallest fanout that reaches `nodes` nodes (the leader included) in
/// two hops: the least F with `F + F*F >= nodes - 1`.
pub fn default_fanout(nodes: usize) -> usize {
    let others = nodes.saturating_sub(1);
    let mut fanout = 1;
    while fanout + fanout * fanout < others {
        fanout += 1;
    }
    fanout
}

/// The order of the tree of `leader`: the leader, then the other nodes by
/// `SHA256(seed || leader || node)` (ties, impossible in practice, by node
/// ID). Duplicates in `nodes` count once; the leader need not be in `nodes`.
pub fn tree_order(seed: &[u8; 32], leader: &NodeId, nodes: &[NodeId]) -> Vec<NodeId> {
    let mut keyed: Vec<([u8; 32], NodeId)> = nodes
        .iter()
        .filter(|node| *node != leader)
        .map(|node| {
            let mut hasher = Sha256::new();
            hasher.update(seed);
            hasher.update(leader);
            hasher.update(node);
            (hasher.finalize().into(), *node)
        })
        .collect();
    keyed.sort();
    keyed.dedup_by(|a, b| a.1 == b.1);
    let mut order = Vec::with_capacity(keyed.len() + 1);
    order.push(*leader);
    order.extend(keyed.into_iter().map(|(_, node)| node));
    order
}

/// Position of `node` in `order`.
fn position(order: &[NodeId], node: &NodeId) -> Option<usize> {
    order.iter().position(|n| n == node)
}

/// The children of `node` in the complete `fanout`-ary tree over `order`
/// (empty for a leaf, and for a node that is not in the order).
pub fn children_of(order: &[NodeId], node: &NodeId, fanout: usize) -> Vec<NodeId> {
    let Some(i) = position(order, node) else {
        return Vec::new();
    };
    children_at(order, i, fanout)
}

fn children_at(order: &[NodeId], i: usize, fanout: usize) -> Vec<NodeId> {
    if fanout == 0 {
        return Vec::new();
    }
    let first = match i.checked_mul(fanout).and_then(|v| v.checked_add(1)) {
        Some(first) if first < order.len() => first,
        _ => return Vec::new(),
    };
    let end = first.saturating_add(fanout).min(order.len());
    order[first..end].to_vec()
}

/// The parent of `node` (none for the root and for a node not in the order).
pub fn parent_of(order: &[NodeId], node: &NodeId, fanout: usize) -> Option<NodeId> {
    let i = position(order, node)?;
    if i == 0 || fanout == 0 {
        return None;
    }
    Some(order[(i - 1) / fanout])
}

/// Hops from the root to `node` (0 for the root).
pub fn depth_of(order: &[NodeId], node: &NodeId, fanout: usize) -> Option<usize> {
    let mut i = position(order, node)?;
    if fanout == 0 {
        return (i == 0).then_some(0);
    }
    let mut depth = 0;
    while i > 0 {
        i = (i - 1) / fanout;
        depth += 1;
    }
    Some(depth)
}

/// In front of a pushed tx-set frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TreeHeader {
    pub kind: FrameKind,
    /// Fanout of the tree (explicit orders: the leader's choice).
    pub fanout: u8,
    /// Hops this copy has made (1 = straight from the leader).
    pub hop: u8,
    pub slot: u32,
    pub leader: NodeId,
    /// When the leader sent it, in Unix milliseconds by the leader's clock.
    pub sent_unix_ms: u64,
    /// Explicit orders only: positions in the node list sorted by node ID,
    /// the leader's first.
    pub order: Vec<u16>,
    /// Explicit orders only: [`TreeConfig::nodes_digest`] of the node list
    /// the positions refer to (zero otherwise). A receiver whose list is
    /// another does not follow the order.
    pub nodes_digest: [u8; TREE_NODES_DIGEST],
}

impl TreeHeader {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(TREE_HEADER_FIXED + 2 * self.order.len());
        out.extend_from_slice(&TREE_TAG.to_be_bytes());
        out.push(self.kind as u8);
        out.push(self.fanout);
        out.push(self.hop);
        out.push(0);
        out.extend_from_slice(&self.slot.to_be_bytes());
        out.extend_from_slice(&self.leader);
        out.extend_from_slice(&self.sent_unix_ms.to_be_bytes());
        out.extend_from_slice(&(self.order.len() as u16).to_be_bytes());
        for index in &self.order {
            out.extend_from_slice(&index.to_be_bytes());
        }
        if self.kind == FrameKind::Explicit {
            out.extend_from_slice(&self.nodes_digest);
        }
        out
    }

    /// Whether a frame starts with the tag (and so is not an ordinary one).
    pub fn is_tagged(frame: &[u8]) -> bool {
        frame.len() >= 4 && frame[..4] == TREE_TAG.to_be_bytes()
    }

    /// Parse the header of a tagged frame. Returns it with the offset at
    /// which the ordinary tx-set frame begins, or `None` if it is malformed.
    pub fn parse(frame: &[u8]) -> Option<(Self, usize)> {
        if !Self::is_tagged(frame) || frame.len() < TREE_HEADER_FIXED {
            return None;
        }
        let kind = match frame[4] {
            1 => FrameKind::Star,
            2 => FrameKind::Hash,
            3 => FrameKind::Explicit,
            _ => return None,
        };
        let fanout = frame[5];
        let hop = frame[6];
        if frame[7] != 0 {
            return None;
        }
        let slot = u32::from_be_bytes(frame[8..12].try_into().unwrap());
        let mut leader = [0u8; 32];
        leader.copy_from_slice(&frame[12..44]);
        let sent_unix_ms = u64::from_be_bytes(frame[44..52].try_into().unwrap());
        let count = u16::from_be_bytes(frame[52..54].try_into().unwrap()) as usize;
        if count > MAX_TREE_NODES {
            return None;
        }
        // Only an explicit order carries one, with the digest of the node
        // list it refers to behind it.
        if (kind == FrameKind::Explicit) != (count != 0) {
            return None;
        }
        let digest_at = TREE_HEADER_FIXED + 2 * count;
        let body = digest_at
            + if kind == FrameKind::Explicit {
                TREE_NODES_DIGEST
            } else {
                0
            };
        if frame.len() < body {
            return None;
        }
        let mut nodes_digest = [0u8; TREE_NODES_DIGEST];
        if kind == FrameKind::Explicit {
            nodes_digest.copy_from_slice(&frame[digest_at..body]);
        }
        let order = (0..count)
            .map(|i| {
                let at = TREE_HEADER_FIXED + 2 * i;
                u16::from_be_bytes(frame[at..at + 2].try_into().unwrap())
            })
            .collect();
        Some((
            Self {
                kind,
                fanout,
                hop,
                slot,
                leader,
                sent_unix_ms,
                order,
                nodes_digest,
            },
            body,
        ))
    }
}

/// What this node's Core told it about the trees of the next slots.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TreeConfig {
    pub mode: TreeMode,
    /// 0 = [`default_fanout`] of the node count.
    pub fanout: u8,
    /// The slots a tree is known for, with the seed of each.
    pub slots: Vec<(u32, [u8; 32])>,
    /// For each of `slots`: the nodes that may lead it, and so root a tree
    /// (the leaders of its first rounds, as Core expects them). `None`: any
    /// node of the tree.
    pub leaders: Vec<Option<Vec<NodeId>>>,
    /// The nodes of the tree, sorted by node ID, without duplicates.
    pub nodes: Vec<NodeId>,
}

impl TreeConfig {
    pub fn new(
        mode: TreeMode,
        fanout: u8,
        slots: Vec<(u32, [u8; 32])>,
        mut nodes: Vec<NodeId>,
    ) -> Self {
        nodes.sort();
        nodes.dedup();
        let leaders = vec![None; slots.len()];
        Self {
            mode,
            fanout,
            slots,
            leaders,
            nodes,
        }
    }

    /// Restrict who may lead each slot (one entry per slot, in order).
    pub fn with_leaders(mut self, leaders: Vec<Option<Vec<NodeId>>>) -> Self {
        assert_eq!(leaders.len(), self.slots.len());
        self.leaders = leaders;
        self
    }

    /// Whether a frame may name `leader` as the root of the tree of `slot`.
    pub fn may_lead(&self, slot: u32, leader: &NodeId) -> bool {
        match self.slots.iter().position(|(s, _)| *s == slot) {
            Some(i) => match &self.leaders[i] {
                Some(leaders) => leaders.contains(leader),
                None => true,
            },
            None => false,
        }
    }

    /// Stands for this node's list of the tree's nodes in a header with an
    /// explicit order: the first bytes of the SHA-256 of the sorted node IDs.
    pub fn nodes_digest(&self) -> [u8; TREE_NODES_DIGEST] {
        let mut hasher = Sha256::new();
        for node in &self.nodes {
            hasher.update(node);
        }
        let digest: [u8; 32] = hasher.finalize().into();
        digest[..TREE_NODES_DIGEST].try_into().unwrap()
    }

    /// Whether a tree of `fanout` over this node's nodes has two levels at
    /// most (a leader that lays out its own tree may not make it deeper).
    pub fn two_levels(&self, fanout: usize) -> bool {
        fanout + fanout.saturating_mul(fanout) >= self.nodes.len().saturating_sub(1)
    }

    pub fn seed_of(&self, slot: u32) -> Option<&[u8; 32]> {
        self.slots
            .iter()
            .find(|(s, _)| *s == slot)
            .map(|(_, seed)| seed)
    }

    /// The fanout of this node's own trees.
    pub fn effective_fanout(&self) -> usize {
        if self.fanout == 0 {
            default_fanout(self.nodes.len())
        } else {
            self.fanout as usize
        }
    }

    pub fn contains(&self, node: &NodeId) -> bool {
        self.nodes.binary_search(node).is_ok()
    }

    /// The order of a hash tree, if the slot is known and the leader is one
    /// of the nodes.
    pub fn hash_order(&self, slot: u32, leader: &NodeId) -> Option<Vec<NodeId>> {
        let seed = self.seed_of(slot)?;
        self.contains(leader)
            .then(|| tree_order(seed, leader, &self.nodes))
    }

    /// The order a header's indices stand for, if they are a permutation of
    /// this node's node list that starts with the leader.
    pub fn explicit_order(&self, header: &TreeHeader) -> Option<Vec<NodeId>> {
        if header.order.len() != self.nodes.len() || header.nodes_digest != self.nodes_digest() {
            return None;
        }
        let mut seen = vec![false; self.nodes.len()];
        let mut order = Vec::with_capacity(self.nodes.len());
        for index in &header.order {
            let index = *index as usize;
            if index >= self.nodes.len() || std::mem::replace(&mut seen[index], true) {
                return None;
            }
            order.push(self.nodes[index]);
        }
        (order.first() == Some(&header.leader)).then_some(order)
    }

    /// The indices that stand for `order` in a header.
    pub fn indices_of(&self, order: &[NodeId]) -> Option<Vec<u16>> {
        order
            .iter()
            .map(|node| self.nodes.binary_search(node).ok().map(|i| i as u16))
            .collect()
    }
}

/// What a node does with a tagged frame beyond filing its set.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RelayPlan {
    /// Pass the bytes on to these nodes.
    Relay(Vec<NodeId>),
    /// Nothing to pass on: a leaf of the tree.
    Leaf,
    /// Not relayed, and why.
    Refused(RelayRefusal),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelayRefusal {
    /// The frame is a star push, or this node does not relay (its mode).
    NotATree,
    /// This node's Core has not described the tree of that slot.
    UnknownSlot,
    /// The leader, this node or the header's order does not fit the nodes
    /// this node knows.
    UnknownNodes,
    /// The sender is not this node's parent in that tree (or did not prove
    /// which validator it is).
    NotParent,
    /// The frame's leader is not one this node's Core expects to lead the
    /// slot.
    NotALeader,
    /// An explicit order reached a node that is not in rtt mode, or makes a
    /// tree of more than two levels.
    NotAllowed,
}

/// Decide whether `me` relays a frame with `header` that came from the
/// validator `sender`.
pub fn relay_plan(
    config: &TreeConfig,
    header: &TreeHeader,
    me: &NodeId,
    sender: Option<&NodeId>,
) -> RelayPlan {
    if !config.mode.relays() || header.kind == FrameKind::Star {
        return RelayPlan::Refused(RelayRefusal::NotATree);
    }
    if config.seed_of(header.slot).is_none() {
        return RelayPlan::Refused(RelayRefusal::UnknownSlot);
    }
    if !config.may_lead(header.slot, &header.leader) {
        return RelayPlan::Refused(RelayRefusal::NotALeader);
    }
    // An order chosen by the sender is followed only where every node is
    // configured for it, and only for the shape a leader is meant to choose:
    // otherwise a validator could send any node's traffic down a chain of
    // its choosing.
    if header.kind == FrameKind::Explicit
        && (config.mode != TreeMode::Rtt || !config.two_levels(header.fanout as usize))
    {
        return RelayPlan::Refused(RelayRefusal::NotAllowed);
    }
    let (order, fanout) = match header.kind {
        FrameKind::Hash => match config.hash_order(header.slot, &header.leader) {
            // The fanout is this node's own: every node of the network is
            // configured alike, and a node that is not refuses below.
            Some(order) => (order, config.effective_fanout()),
            None => return RelayPlan::Refused(RelayRefusal::UnknownNodes),
        },
        FrameKind::Explicit => match config.explicit_order(header) {
            Some(order) if header.fanout != 0 => (order, header.fanout as usize),
            _ => return RelayPlan::Refused(RelayRefusal::UnknownNodes),
        },
        FrameKind::Star => unreachable!(),
    };
    if !config.contains(me) {
        return RelayPlan::Refused(RelayRefusal::UnknownNodes);
    }
    match (parent_of(&order, me, fanout), sender) {
        (Some(parent), Some(sender)) if parent == *sender => {}
        _ => return RelayPlan::Refused(RelayRefusal::NotParent),
    }
    let children = children_of(&order, me, fanout);
    if children.is_empty() {
        RelayPlan::Leaf
    } else {
        RelayPlan::Relay(children)
    }
}

/// Two nodes whose round-trip times differ by no more than this, or by no
/// more than a fifth of the larger one, are in one group. Groups are chains:
/// a run of nodes each that close to the next is one group.
const RTT_GROUP_GAP: Duration = Duration::from_millis(8);

/// Split nodes sorted by round-trip time into groups at the gaps.
fn rtt_groups(mut measured: Vec<(Duration, NodeId)>) -> Vec<Vec<NodeId>> {
    measured.sort();
    let mut groups: Vec<Vec<NodeId>> = Vec::new();
    let mut last: Option<Duration> = None;
    for (rtt, node) in measured {
        let same = last.is_some_and(|prev| {
            let gap = rtt.saturating_sub(prev);
            gap <= RTT_GROUP_GAP || gap <= rtt / 5
        });
        if same {
            groups.last_mut().unwrap().push(node);
        } else {
            groups.push(vec![node]);
        }
        last = Some(rtt);
    }
    groups
}

/// An order for the leader's own tree from its round-trip times: nodes at
/// about the same distance are taken to be in one place; every such group
/// gets relays in proportion to its size, and a relay's children come from
/// its own group. So a set makes one long hop to a group's relay and a short
/// one from there, instead of two hops of any length.
///
/// `nodes` are the other nodes of the tree with the round-trip time to each
/// (`None`: not measured; those form a group of their own, served last).
/// The result starts with `leader` and holds every node once.
pub fn rtt_order(
    leader: &NodeId,
    nodes: &[(NodeId, Option<Duration>)],
    fanout: usize,
) -> Vec<NodeId> {
    let mut unique: HashMap<NodeId, Option<Duration>> = HashMap::new();
    for (node, rtt) in nodes {
        if node != leader {
            unique.entry(*node).or_insert(*rtt);
        }
    }
    let mut measured = Vec::new();
    let mut unmeasured = Vec::new();
    for (node, rtt) in unique {
        match rtt {
            Some(rtt) => measured.push((rtt, node)),
            None => unmeasured.push(node),
        }
    }
    unmeasured.sort();
    let mut groups = rtt_groups(measured);
    if !unmeasured.is_empty() {
        groups.push(unmeasured);
    }
    let total: usize = groups.iter().map(Vec::len).sum();
    let mut order = vec![*leader];
    if total == 0 {
        return order;
    }
    let fanout = fanout.max(1);
    let relays = fanout.min(total);
    // A tree of two levels holds `relays` relays and `relays * fanout`
    // leaves. If the nodes do not fit, or there are more groups than relays,
    // the grouping cannot be kept: fall back to the nearest nodes first.
    if total > relays + relays * fanout || groups.len() > relays {
        order.extend(groups.into_iter().flatten());
        return order;
    }
    // Relays per group: one each, the rest by the largest remainder of each
    // group's share, never more than the group's size.
    let mut seats: Vec<usize> = vec![1; groups.len()];
    let mut left = relays - groups.len();
    while left > 0 {
        // The group with the most nodes per relay gets the next one.
        let Some(next) = (0..groups.len())
            .filter(|g| seats[*g] < groups[*g].len())
            .max_by(|a, b| {
                let load = |g: usize| groups[g].len() as f64 / seats[g] as f64;
                load(*a)
                    .partial_cmp(&load(*b))
                    .unwrap()
                    // Equal load: the nearer group (the lower index).
                    .then(b.cmp(a))
            })
        else {
            break;
        };
        seats[next] += 1;
        left -= 1;
    }
    // Level 1: each group's relays are its nearest nodes.
    let mut relay_nodes: Vec<NodeId> = Vec::new();
    let mut owner_group: Vec<usize> = Vec::new();
    let mut rest: Vec<std::collections::VecDeque<NodeId>> = Vec::new();
    for (g, group) in groups.iter().enumerate() {
        let (heads, others) = group.split_at(seats[g]);
        for node in heads {
            relay_nodes.push(*node);
            owner_group.push(g);
        }
        rest.push(others.iter().copied().collect());
    }
    let relay_count = relay_nodes.len();
    // Level 2: every relay's children come from its own group, dealt out in
    // turn so that a group's relays share it evenly.
    let mut children: Vec<Vec<NodeId>> = vec![Vec::new(); relay_count];
    for (g, queue) in rest.iter_mut().enumerate() {
        let mine: Vec<usize> = (0..relay_count).filter(|j| owner_group[*j] == g).collect();
        let mut turn = 0;
        while !queue.is_empty() && mine.iter().any(|j| children[*j].len() < fanout) {
            let j = mine[turn % mine.len()];
            turn += 1;
            if children[j].len() < fanout {
                children[j].push(queue.pop_front().unwrap());
            }
        }
    }
    // What did not fit under its own group's relays goes where there is
    // room (a relay with its own ten nodes serves a group of ten; larger
    // groups overflow).
    let mut leftovers: std::collections::VecDeque<NodeId> = rest.into_iter().flatten().collect();
    for slot in children.iter_mut() {
        while slot.len() < fanout {
            match leftovers.pop_front() {
                Some(node) => slot.push(node),
                None => break,
            }
        }
    }
    debug_assert!(leftovers.is_empty());
    // In the complete tree, position i's children are the positions
    // F*i+1 ..= F*i+F, so only the last relays may have fewer than F
    // children. Put the relays with a full set of children first; if more
    // than one is short, the later ones hand children to the earlier.
    let mut by_fullness: Vec<usize> = (0..relay_count).collect();
    by_fullness.sort_by_key(|j| children[*j].len() < fanout);
    for at in 0..relay_count {
        let j = by_fullness[at];
        while children[j].len() < fanout {
            let donor = by_fullness[at + 1..]
                .iter()
                .rev()
                .copied()
                .find(|k| !children[*k].is_empty());
            match donor {
                Some(k) => {
                    let node = children[k].pop().unwrap();
                    children[j].push(node);
                }
                None => break,
            }
        }
    }
    order.extend(by_fullness.iter().map(|j| relay_nodes[*j]));
    for j in &by_fullness {
        order.extend(children[*j].iter().copied());
    }
    order
}

/// How long a node that expects a set through the tree waits, from the first
/// mention of the set's hash, before it asks a peer for it. Long enough for
/// two hops of a large set between the farthest regions; short against
/// Core's own retry (one second).
pub const TREE_FETCH_GRACE: Duration = Duration::from_millis(300);

/// Frames a node remembers having relayed, hashes it waits for, and sets it
/// saw arrive.
const TREE_MEMORY: usize = 256;

/// Round-trip samples kept per node; the smallest counts (a sample taken
/// behind a large transfer on the same stream is too long, never too short).
const RTT_SAMPLES: usize = 8;

/// What to do about a set this node does not hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FetchDecision {
    /// Ask a peer now.
    Proceed,
    /// The tree should bring it: ask only if it is still missing after this
    /// long (the caller sets the timer).
    Wait(Duration),
    /// A timer for this set is already running.
    AlreadyWaiting,
}

type RelayKey = (u32, NodeId, usize, [u8; 32]);

/// What a node does with a frame it could relay.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelayAdmission {
    Relay,
    /// A copy of a frame this node relayed already.
    Duplicate,
    /// The node relayed [`MAX_RELAYS_PER_SLOT_AND_LEADER`] other frames of
    /// this slot and leader already.
    OverLimit,
}

/// A node's view of the trees: what its Core told it, what it relayed, what
/// it waits for, and how far away its peers are.
pub struct TreeState {
    config: Option<TreeConfig>,
    relayed: lru::LruCache<RelayKey, ()>,
    waiting: lru::LruCache<[u8; 32], std::time::Instant>,
    delivered: lru::LruCache<[u8; 32], ()>,
    rtts: HashMap<NodeId, std::collections::VecDeque<Duration>>,
    /// This node's own validator key, once Core has sent it.
    local: Option<NodeId>,
    /// Frames relayed per slot and leader.
    relays: lru::LruCache<(u32, NodeId), u8>,
    /// Relays not yet handed to the transport, per child.
    relays_in_flight: HashMap<NodeId, u32>,
    /// The probe this node last sent each peer and has no answer to.
    probes: HashMap<NodeId, u64>,
    #[cfg(test)]
    probing_paused: bool,
}

impl Default for TreeState {
    fn default() -> Self {
        Self::new()
    }
}

impl TreeState {
    pub fn new() -> Self {
        let memory = std::num::NonZeroUsize::new(TREE_MEMORY).unwrap();
        Self {
            config: None,
            relayed: lru::LruCache::new(memory),
            waiting: lru::LruCache::new(memory),
            delivered: lru::LruCache::new(memory),
            rtts: HashMap::new(),
            local: None,
            relays: lru::LruCache::new(memory),
            relays_in_flight: HashMap::new(),
            probes: HashMap::new(),
            #[cfg(test)]
            probing_paused: false,
        }
    }

    pub fn set_local_node(&mut self, node: NodeId) {
        self.local = Some(node);
    }

    pub fn set_config(&mut self, config: TreeConfig) {
        self.config = Some(config);
    }

    pub fn config(&self) -> Option<&TreeConfig> {
        self.config.as_ref()
    }

    pub fn mode(&self) -> TreeMode {
        self.config.as_ref().map_or(TreeMode::Off, |c| c.mode)
    }

    /// Whether this node relays this copy of a frame: only the first copy
    /// it sees, and only [`MAX_RELAYS_PER_SLOT_AND_LEADER`] different frames
    /// of a slot and leader. Copies are told apart by slot, leader, length
    /// and the first and last bytes of the body, without hashing megabytes
    /// before the bytes go on; a sender that varies those bytes runs into
    /// the limit.
    pub fn admit_relay(&mut self, header: &TreeHeader, body: &[u8]) -> RelayAdmission {
        let mut ends = [0u8; 32];
        let head = body.len().min(16);
        ends[..head].copy_from_slice(&body[..head]);
        let tail = body.len().min(16);
        ends[32 - tail..].copy_from_slice(&body[body.len() - tail..]);
        let key = (header.slot, header.leader, body.len(), ends);
        if self.relayed.contains(&key) {
            return RelayAdmission::Duplicate;
        }
        let count = self
            .relays
            .get_or_insert_mut((header.slot, header.leader), || 0);
        if *count >= MAX_RELAYS_PER_SLOT_AND_LEADER {
            return RelayAdmission::OverLimit;
        }
        *count += 1;
        self.relayed.put(key, ());
        RelayAdmission::Relay
    }

    /// A relay to `child` is about to be written. False: an earlier one is
    /// still in flight, and this one is not sent (the child asks instead).
    pub fn start_relay(&mut self, child: NodeId) -> bool {
        let in_flight = self.relays_in_flight.entry(child).or_insert(0);
        if *in_flight >= MAX_IN_FLIGHT_RELAYS_PER_CHILD {
            return false;
        }
        *in_flight += 1;
        true
    }

    /// The relay to `child` was handed to the transport, or failed.
    pub fn finish_relay(&mut self, child: &NodeId) {
        if let Some(count) = self.relays_in_flight.get_mut(child) {
            *count = count.saturating_sub(1);
            if *count == 0 {
                self.relays_in_flight.remove(child);
            }
        }
    }

    /// This node sends `node` a probe with `value`.
    pub fn probe_sent(&mut self, node: NodeId, value: u64) {
        self.probes.insert(node, value);
    }

    /// `node` answered with `value`. True if that is the probe this node
    /// last sent it and has no answer to yet: only then is the answer a
    /// measurement (a peer cannot make itself look near with an answer
    /// nobody asked for).
    pub fn probe_answered(&mut self, node: &NodeId, value: u64) -> bool {
        if self.probes.get(node) == Some(&value) {
            self.probes.remove(node);
            true
        } else {
            false
        }
    }

    /// A proposal (or Core) named the set `hash` of `slot`, and this node
    /// does not hold it.
    pub fn fetch_decision(
        &mut self,
        hash: &[u8; 32],
        slot: u32,
        now: std::time::Instant,
    ) -> FetchDecision {
        // Only a node of the tree is sent the set through it. (A node that
        // is not, a watcher for one, asks at once as before.)
        let local = self.local;
        let expects_tree = self.config.as_ref().is_some_and(|c| {
            c.mode.relays() && c.seed_of(slot).is_some() && local.is_some_and(|me| c.contains(&me))
        });
        if !expects_tree || self.delivered.contains(hash) {
            return FetchDecision::Proceed;
        }
        match self.waiting.get(hash) {
            Some(deadline) if *deadline > now => FetchDecision::AlreadyWaiting,
            Some(_) => FetchDecision::Proceed,
            None => {
                self.waiting.put(*hash, now + TREE_FETCH_GRACE);
                FetchDecision::Wait(TREE_FETCH_GRACE)
            }
        }
    }

    /// The set `hash` arrived from a peer.
    pub fn set_delivered(&mut self, hash: [u8; 32]) {
        self.delivered.put(hash, ());
    }

    pub fn is_delivered(&self, hash: &[u8; 32]) -> bool {
        self.delivered.contains(hash)
    }

    /// Tests set round-trip times by hand; the probe task then leaves them
    /// alone.
    #[cfg(test)]
    pub fn pause_probing(&mut self) {
        self.probing_paused = true;
    }

    /// Whether the probe task measures (always, outside tests).
    pub fn probing(&self) -> bool {
        #[cfg(test)]
        if self.probing_paused {
            return false;
        }
        true
    }

    /// A round trip to the validator `node` took `rtt`.
    pub fn note_rtt(&mut self, node: NodeId, rtt: Duration) {
        let samples = self.rtts.entry(node).or_default();
        if samples.len() == RTT_SAMPLES {
            samples.pop_front();
        }
        samples.push_back(rtt);
    }

    /// The peer is gone; a new connection is measured afresh.
    pub fn forget_rtt(&mut self, node: &NodeId) {
        self.rtts.remove(node);
        self.probes.remove(node);
    }

    /// The round-trip time to `node`: the smallest of the last samples.
    pub fn rtt_of(&self, node: &NodeId) -> Option<Duration> {
        self.rtts.get(node).and_then(|s| s.iter().min().copied())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn node(n: u16) -> NodeId {
        let mut id = [0u8; 32];
        id[..2].copy_from_slice(&n.to_be_bytes());
        id[31] = 0xAA;
        id
    }

    fn nodes(count: u16) -> Vec<NodeId> {
        (0..count).map(node).collect()
    }

    #[test]
    fn default_fanout_reaches_everyone_in_two_hops() {
        for (count, fanout) in [
            (1, 1),
            (2, 1),
            (3, 1),
            (4, 2),
            (7, 2),
            (8, 3),
            (13, 3),
            (48, 7),
            (90, 9),
            (91, 9),
            (92, 10),
        ] {
            assert_eq!(default_fanout(count), fanout, "{count} nodes");
            let f = default_fanout(count);
            assert!(f + f * f >= count - 1);
            if f > 1 {
                assert!((f - 1) + (f - 1) * (f - 1) < count - 1);
            }
        }
    }

    #[test]
    fn the_order_starts_with_the_leader_and_depends_on_seed_and_leader() {
        let all = nodes(90);
        let seed = [7u8; 32];
        let order = tree_order(&seed, &all[5], &all);
        assert_eq!(order.len(), 90);
        assert_eq!(order[0], all[5]);
        let mut sorted = order.clone();
        sorted.sort();
        assert_eq!(sorted, all);
        // The same for every node that agrees on the inputs, whatever order
        // (and duplicates) the nodes come in.
        let mut shuffled = all.clone();
        shuffled.reverse();
        shuffled.push(all[3]);
        assert_eq!(tree_order(&seed, &all[5], &shuffled), order);
        // Another seed or another leader gives another order.
        assert_ne!(tree_order(&[8u8; 32], &all[5], &all), order);
        let other = tree_order(&seed, &all[6], &all);
        assert_eq!(other[0], all[6]);
        assert_ne!(other[1..], order[1..]);
        // A leader that is not among the nodes still roots its tree.
        let outsider = node(999);
        let with_outsider = tree_order(&seed, &outsider, &all);
        assert_eq!(with_outsider.len(), 91);
        assert_eq!(with_outsider[0], outsider);
    }

    #[test]
    fn parents_and_children_form_one_tree_of_least_depth() {
        for (count, fanout) in [(90u16, 9usize), (90, 4), (90, 1), (48, 7), (2, 3), (1, 3)] {
            let all = nodes(count);
            let order = tree_order(&[1u8; 32], &all[0], &all);
            let mut reached = vec![order[0]];
            let mut frontier = vec![order[0]];
            let mut depth = 0;
            while !frontier.is_empty() {
                let mut next = Vec::new();
                for parent in &frontier {
                    let children = children_of(&order, parent, fanout);
                    assert!(children.len() <= fanout);
                    for child in &children {
                        assert_eq!(parent_of(&order, child, fanout), Some(*parent));
                        assert_eq!(depth_of(&order, child, fanout), Some(depth + 1));
                        assert!(!reached.contains(child));
                        reached.push(*child);
                    }
                    next.extend(children);
                }
                if !next.is_empty() {
                    depth += 1;
                }
                frontier = next;
            }
            // Everyone is reached exactly once.
            assert_eq!(reached.len(), count as usize);
            // No tree of that fanout is shallower: the levels above the last
            // are full.
            let mut capacity = 1usize;
            let mut level = 1usize;
            let mut least = 0;
            while capacity < count as usize {
                level *= fanout;
                capacity += level;
                least += 1;
            }
            assert_eq!(depth, least, "{count} nodes, fanout {fanout}");
            assert_eq!(parent_of(&order, &order[0], fanout), None);
            assert_eq!(depth_of(&order, &order[0], fanout), Some(0));
        }
        let all = nodes(10);
        let order = tree_order(&[1u8; 32], &all[0], &all);
        assert!(children_of(&order, &node(77), 3).is_empty());
        assert_eq!(parent_of(&order, &node(77), 3), None);
        assert!(children_of(&order, &order[0], 0).is_empty());
    }

    fn header(kind: FrameKind, slot: u32, leader: NodeId) -> TreeHeader {
        TreeHeader {
            kind,
            fanout: 3,
            hop: 1,
            slot,
            leader,
            sent_unix_ms: 1_800_000_000_123,
            order: Vec::new(),
            nodes_digest: [0; TREE_NODES_DIGEST],
        }
    }

    #[test]
    fn a_header_round_trips_and_malformed_ones_are_refused() {
        let mut h = header(FrameKind::Hash, 77, node(3));
        let mut frame = h.encode();
        assert_eq!(frame.len(), TREE_HEADER_FIXED);
        frame.extend_from_slice(b"body");
        assert!(TreeHeader::is_tagged(&frame));
        let (parsed, body) = TreeHeader::parse(&frame).unwrap();
        assert_eq!(parsed, h);
        assert_eq!(&frame[body..], b"body");

        h.kind = FrameKind::Explicit;
        h.order = vec![3, 0, 1, 2];
        h.nodes_digest = [1, 2, 3, 4, 5, 6, 7, 8];
        let mut frame = h.encode();
        frame.extend_from_slice(b"x");
        let (parsed, body) = TreeHeader::parse(&frame).unwrap();
        assert_eq!(parsed, h);
        assert_eq!(body, TREE_HEADER_FIXED + 8 + TREE_NODES_DIGEST);
        // The largest header fits the limit the reader allows for it.
        let mut largest = h.clone();
        largest.order = (0..MAX_TREE_NODES as u16).collect();
        assert_eq!(largest.encode().len(), MAX_TREE_HEADER);

        // An ordinary frame starts with a length and is not tagged.
        assert!(!TreeHeader::is_tagged(&[0, 0x10, 0, 0, 1, 2, 3]));
        assert!(TreeHeader::parse(&[0, 0x10, 0, 0, 1, 2, 3]).is_none());
        // Truncated, unknown kind, an order where none belongs, none where
        // one does, an order longer than the frame.
        let good = header(FrameKind::Hash, 1, node(1)).encode();
        assert!(TreeHeader::parse(&good[..good.len() - 1]).is_none());
        let mut bad = good.clone();
        bad[4] = 9;
        assert!(TreeHeader::parse(&bad).is_none());
        // The reserved byte must be zero.
        let mut bad = good.clone();
        bad[7] = 1;
        assert!(TreeHeader::parse(&bad).is_none());
        let mut bad = good.clone();
        bad[53] = 2;
        bad.extend_from_slice(&[0, 0, 0, 1]);
        assert!(TreeHeader::parse(&bad).is_none());
        let mut explicit = header(FrameKind::Explicit, 1, node(1));
        explicit.order = Vec::new();
        assert!(TreeHeader::parse(&explicit.encode()).is_none());
        explicit.order = vec![0, 1];
        let encoded = explicit.encode();
        assert!(TreeHeader::parse(&encoded[..encoded.len() - 1]).is_none());
    }

    fn config(mode: TreeMode, fanout: u8, count: u16) -> TreeConfig {
        TreeConfig::new(
            mode,
            fanout,
            vec![(10, [1u8; 32]), (11, [2u8; 32])],
            nodes(count),
        )
    }

    #[test]
    fn a_node_relays_only_what_its_parent_sent_for_a_tree_it_knows() {
        let cfg = config(TreeMode::Hash, 3, 20);
        let leader = node(4);
        let order = cfg.hash_order(10, &leader).unwrap();
        let h = header(FrameKind::Hash, 10, leader);
        // A child of the root relays to its own children.
        let child = order[1];
        let expected = children_of(&order, &child, 3);
        assert_eq!(expected.len(), 3);
        assert_eq!(
            relay_plan(&cfg, &h, &child, Some(&leader)),
            RelayPlan::Relay(expected)
        );
        // A leaf has nothing to pass on.
        let leaf = order[19];
        let parent = parent_of(&order, &leaf, 3).unwrap();
        assert_eq!(relay_plan(&cfg, &h, &leaf, Some(&parent)), RelayPlan::Leaf);
        // From anyone but the parent: refused. Also from a peer that did not
        // prove which validator it is.
        assert_eq!(
            relay_plan(&cfg, &h, &child, Some(&order[2])),
            RelayPlan::Refused(RelayRefusal::NotParent)
        );
        assert_eq!(
            relay_plan(&cfg, &h, &child, None),
            RelayPlan::Refused(RelayRefusal::NotParent)
        );
        // The root itself never relays what comes back to it.
        assert_eq!(
            relay_plan(&cfg, &h, &leader, Some(&child)),
            RelayPlan::Refused(RelayRefusal::NotParent)
        );
        // A slot this node's Core has not described.
        assert_eq!(
            relay_plan(
                &cfg,
                &header(FrameKind::Hash, 12, leader),
                &child,
                Some(&leader)
            ),
            RelayPlan::Refused(RelayRefusal::UnknownSlot)
        );
        // A leader, or a relay, that is not one of the nodes.
        assert_eq!(
            relay_plan(
                &cfg,
                &header(FrameKind::Hash, 10, node(500)),
                &child,
                Some(&node(500))
            ),
            RelayPlan::Refused(RelayRefusal::UnknownNodes)
        );
        assert_eq!(
            relay_plan(&cfg, &h, &node(500), Some(&leader)),
            RelayPlan::Refused(RelayRefusal::UnknownNodes)
        );
        // Star frames, and nodes whose mode does not relay.
        assert_eq!(
            relay_plan(
                &cfg,
                &header(FrameKind::Star, 10, leader),
                &child,
                Some(&leader)
            ),
            RelayPlan::Refused(RelayRefusal::NotATree)
        );
        for mode in [TreeMode::Off, TreeMode::Star] {
            assert_eq!(
                relay_plan(&config(mode, 3, 20), &h, &child, Some(&leader)),
                RelayPlan::Refused(RelayRefusal::NotATree)
            );
        }
        // A frame whose leader this node's Core does not expect to lead the
        // slot, also from the node that would be the parent in that tree.
        let restricted =
            config(TreeMode::Hash, 3, 20).with_leaders(vec![Some(vec![node(7), node(8)]), None]);
        assert!(!restricted.may_lead(10, &leader));
        assert!(restricted.may_lead(10, &node(8)));
        assert!(restricted.may_lead(11, &leader));
        assert!(!restricted.may_lead(12, &leader));
        assert_eq!(
            relay_plan(&restricted, &h, &child, Some(&leader)),
            RelayPlan::Refused(RelayRefusal::NotALeader)
        );
        let h8 = header(FrameKind::Hash, 10, node(8));
        let order8 = restricted.hash_order(10, &node(8)).unwrap();
        assert_eq!(
            relay_plan(&restricted, &h8, &order8[1], Some(&node(8))),
            RelayPlan::Relay(children_of(&order8, &order8[1], 3))
        );
        // The second slot has another seed, so another tree.
        assert_ne!(cfg.hash_order(11, &leader).unwrap(), order);
        // A fanout of 0 is the default for the node count.
        assert_eq!(config(TreeMode::Hash, 0, 90).effective_fanout(), 9);
    }

    #[test]
    fn an_explicit_order_is_followed_if_it_is_a_permutation_led_by_the_leader() {
        let cfg = config(TreeMode::Rtt, 0, 8);
        let leader = node(2);
        // leader, then 7 6 5 | 4 3 1 | 0
        let order: Vec<NodeId> = [2u16, 7, 6, 5, 4, 3, 1, 0]
            .iter()
            .map(|n| node(*n))
            .collect();
        let mut h = header(FrameKind::Explicit, 10, leader);
        h.fanout = 3;
        h.order = cfg.indices_of(&order).unwrap();
        h.nodes_digest = cfg.nodes_digest();
        assert_eq!(cfg.explicit_order(&h).unwrap(), order);
        // node 7 is the first child of the root; its children are 4, 3, 1.
        assert_eq!(
            relay_plan(&cfg, &h, &node(7), Some(&leader)),
            RelayPlan::Relay(vec![node(4), node(3), node(1)])
        );
        assert_eq!(
            relay_plan(&cfg, &h, &node(6), Some(&leader)),
            RelayPlan::Relay(vec![node(0)])
        );
        assert_eq!(
            relay_plan(&cfg, &h, &node(4), Some(&node(7))),
            RelayPlan::Leaf
        );
        assert_eq!(
            relay_plan(&cfg, &h, &node(4), Some(&node(6))),
            RelayPlan::Refused(RelayRefusal::NotParent)
        );
        // Not a permutation, too short, not led by the leader, an index out
        // of range.
        let mut bad = h.clone();
        bad.order[3] = bad.order[4];
        assert_eq!(
            relay_plan(&cfg, &bad, &node(7), Some(&leader)),
            RelayPlan::Refused(RelayRefusal::UnknownNodes)
        );
        let mut bad = h.clone();
        bad.order.pop();
        assert!(cfg.explicit_order(&bad).is_none());
        let mut bad = h.clone();
        bad.order.swap(0, 1);
        assert!(cfg.explicit_order(&bad).is_none());
        let mut bad = h.clone();
        bad.order[1] = 99;
        assert!(cfg.explicit_order(&bad).is_none());
        // An order for another list of nodes (the same number of them).
        let mut bad = h.clone();
        bad.nodes_digest[0] ^= 1;
        assert_eq!(
            relay_plan(&cfg, &bad, &node(7), Some(&leader)),
            RelayPlan::Refused(RelayRefusal::UnknownNodes)
        );
        let mut other_nodes = nodes(7);
        other_nodes.push(node(100));
        assert_ne!(
            TreeConfig::new(TreeMode::Rtt, 0, vec![(10, [1u8; 32])], other_nodes).nodes_digest(),
            cfg.nodes_digest()
        );
        // A leader lays out two levels, not a chain: a fanout that needs
        // more is refused (8 nodes need a fanout of 3).
        assert!(cfg.two_levels(3));
        assert!(!cfg.two_levels(2));
        for fanout in [0u8, 1, 2] {
            let mut deep = h.clone();
            deep.fanout = fanout;
            assert_eq!(
                relay_plan(&cfg, &deep, &node(7), Some(&leader)),
                RelayPlan::Refused(RelayRefusal::NotAllowed)
            );
        }
        // A node that is not in rtt mode does not follow a sender's order.
        let hash_cfg = config(TreeMode::Hash, 0, 8);
        assert_eq!(
            relay_plan(&hash_cfg, &h, &node(7), Some(&leader)),
            RelayPlan::Refused(RelayRefusal::NotAllowed)
        );
        // A node in rtt mode follows the common tree as well.
        let hash_frame = header(FrameKind::Hash, 10, leader);
        let common = cfg.hash_order(10, &leader).unwrap();
        assert_eq!(
            relay_plan(&cfg, &hash_frame, &common[1], Some(&leader)),
            RelayPlan::Relay(children_of(&common, &common[1], cfg.effective_fanout()))
        );
    }

    /// Four places, as on perfnet: 22 nodes with the leader (21 others), and
    /// 23, 22, 23 at three distances.
    fn four_regions() -> (NodeId, Vec<(NodeId, Option<Duration>)>, Vec<Vec<NodeId>>) {
        let leader = node(0);
        let mut measured = Vec::new();
        let mut regions = Vec::new();
        let mut next = 1u16;
        for (size, base_ms) in [(21u16, 1u64), (23, 62), (22, 91), (23, 215)] {
            let mut region = Vec::new();
            for i in 0..size {
                let n = node(next);
                next += 1;
                // A few milliseconds of spread inside a region.
                let rtt = Duration::from_micros(base_ms * 1000 + (i as u64 * 137) % 2500);
                measured.push((n, Some(rtt)));
                region.push(n);
            }
            regions.push(region);
        }
        (leader, measured, regions)
    }

    #[test]
    fn an_rtt_order_keeps_a_relays_children_in_its_own_group() {
        let (leader, mut measured, regions) = four_regions();
        // The input order must not matter.
        measured.reverse();
        let fanout = default_fanout(90);
        assert_eq!(fanout, 9);
        let order = rtt_order(&leader, &measured, fanout);
        assert_eq!(order.len(), 90);
        assert_eq!(order[0], leader);
        let mut sorted = order.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), 90);

        let region_of = |n: &NodeId| regions.iter().position(|r| r.contains(n)).unwrap();
        let relays = children_of(&order, &leader, fanout);
        assert_eq!(relays.len(), 9);
        // Every region has at least two relays (9 relays for 4 regions of
        // about equal size).
        for r in 0..4 {
            let count = relays.iter().filter(|n| region_of(n) == r).count();
            assert!(count >= 2, "region {r} has {count} relays");
        }
        // Nearly every node is a relay or sits under a relay of its region.
        let mut foreign = 0;
        for relay in &relays {
            for child in children_of(&order, relay, fanout) {
                assert_eq!(depth_of(&order, &child, fanout), Some(2));
                if region_of(&child) != region_of(relay) {
                    foreign += 1;
                }
            }
        }
        // A relay and its nine children are ten nodes; regions of 21 to 23
        // overflow by one to three, and the overflow sits under the relays
        // of the region that has a third relay.
        assert!(
            foreign <= 7,
            "{foreign} nodes under a relay of another region"
        );
        // And the tree is still complete: two hops for everyone.
        for n in &order[1..] {
            assert!(depth_of(&order, n, fanout).unwrap() <= 2);
        }
    }

    #[test]
    fn an_rtt_order_copes_with_unmeasured_nodes_small_networks_and_many_groups() {
        // Nothing measured: one group, still every node once.
        let leader = node(0);
        let unmeasured: Vec<_> = (1..20u16).map(|n| (node(n), None)).collect();
        let order = rtt_order(&leader, &unmeasured, default_fanout(20));
        assert_eq!(order.len(), 20);
        assert_eq!(order[0], leader);
        // Alone.
        assert_eq!(rtt_order(&leader, &[], 3), vec![leader]);
        // The leader in its own list, and duplicates, count once.
        let dup = vec![
            (leader, Some(Duration::from_millis(1))),
            (node(1), Some(Duration::from_millis(5))),
            (node(1), Some(Duration::from_millis(50))),
        ];
        assert_eq!(rtt_order(&leader, &dup, 2), vec![leader, node(1)]);
        // More groups than relays: nearest first, every node once.
        let spread: Vec<_> = (1..=12u16)
            .map(|n| (node(n), Some(Duration::from_millis(n as u64 * 40))))
            .collect();
        let order = rtt_order(&leader, &spread, 3);
        assert_eq!(order.len(), 13);
        assert_eq!(order[1], node(1));
        let mut sorted = order.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), 13);
        // A fanout too small for two levels: nearest first as well.
        let (leader, measured, _) = four_regions();
        let order = rtt_order(&leader, &measured, 4);
        assert_eq!(order.len(), 90);
        let mut sorted = order.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), 90);
        // Mixed: measured groups first, the unmeasured ones are still placed.
        let mut mixed = four_regions().1;
        mixed[5].1 = None;
        mixed[40].1 = None;
        let order = rtt_order(&leader, &mixed, 9);
        assert_eq!(order.len(), 90);
        let mut sorted = order.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), 90);
    }

    #[test]
    fn a_probe_round_trips() {
        for probe in [
            Probe::Ping(0),
            Probe::Ping(u64::MAX),
            Probe::Pong(123_456_789),
        ] {
            assert_eq!(Probe::parse(&probe.encode()), Some(Some(probe)));
        }
        // Not a probe at all; a probe of the wrong length or kind.
        assert_eq!(Probe::parse(&[0, 0, 1, 0, 9, 9]), None);
        assert_eq!(Probe::parse(&TREE_TAG.to_be_bytes()), None);
        let mut bad = Probe::Ping(5).encode().to_vec();
        bad.push(0);
        assert_eq!(Probe::parse(&bad), Some(None));
        let mut bad = Probe::Ping(5).encode();
        bad[4] = 3;
        assert_eq!(Probe::parse(&bad), Some(None));
        // A probe is not mistaken for a tree frame.
        assert!(!TreeHeader::is_tagged(&Probe::Ping(1).encode()));
    }

    #[test]
    fn a_node_that_expects_the_tree_waits_once_before_it_asks() {
        let mut state = TreeState::new();
        let now = std::time::Instant::now();
        let hash = [9u8; 32];
        // No tree: ask at once.
        assert_eq!(state.fetch_decision(&hash, 10, now), FetchDecision::Proceed);
        for mode in [TreeMode::Off, TreeMode::Star] {
            state.set_config(config(mode, 0, 20));
            assert_eq!(state.fetch_decision(&hash, 10, now), FetchDecision::Proceed);
        }
        state.set_config(config(TreeMode::Hash, 0, 20));
        assert_eq!(state.mode(), TreeMode::Hash);
        // A node that does not know which validator it is, or is not one of
        // the tree's nodes, is not sent the set through the tree: no wait.
        assert_eq!(state.fetch_decision(&hash, 10, now), FetchDecision::Proceed);
        state.set_local_node(node(500));
        assert_eq!(state.fetch_decision(&hash, 10, now), FetchDecision::Proceed);
        state.set_local_node(node(3));
        // A slot the tree is not known for (catching up): ask at once.
        assert_eq!(state.fetch_decision(&hash, 9, now), FetchDecision::Proceed);
        // Otherwise wait, once.
        assert_eq!(
            state.fetch_decision(&hash, 10, now),
            FetchDecision::Wait(TREE_FETCH_GRACE)
        );
        assert_eq!(
            state.fetch_decision(&hash, 10, now + Duration::from_millis(100)),
            FetchDecision::AlreadyWaiting
        );
        assert_eq!(
            state.fetch_decision(&hash, 10, now + TREE_FETCH_GRACE),
            FetchDecision::Proceed
        );
        // Another set has its own wait.
        assert_eq!(
            state.fetch_decision(&[8u8; 32], 11, now),
            FetchDecision::Wait(TREE_FETCH_GRACE)
        );
        // A set that arrived is not waited for.
        assert!(!state.is_delivered(&[7u8; 32]));
        state.set_delivered([7u8; 32]);
        assert!(state.is_delivered(&[7u8; 32]));
        assert_eq!(
            state.fetch_decision(&[7u8; 32], 10, now),
            FetchDecision::Proceed
        );
    }

    #[test]
    fn a_frame_is_relayed_once_and_two_frames_per_slot_and_leader() {
        let mut state = TreeState::new();
        let h = header(FrameKind::Hash, 10, node(1));
        let body = vec![5u8; 100];
        assert_eq!(state.admit_relay(&h, &body), RelayAdmission::Relay);
        assert_eq!(state.admit_relay(&h, &body), RelayAdmission::Duplicate);
        // Another body is another frame: the second one of this slot and
        // leader, and the last.
        let mut other = body.clone();
        other[99] = 6;
        assert_eq!(state.admit_relay(&h, &other), RelayAdmission::Relay);
        assert_eq!(state.admit_relay(&h, &other), RelayAdmission::Duplicate);
        assert_eq!(
            state.admit_relay(&h, &body[..50]),
            RelayAdmission::OverLimit
        );
        other[0] = 6;
        assert_eq!(state.admit_relay(&h, &other), RelayAdmission::OverLimit);
        // The first copies are still known as copies.
        assert_eq!(state.admit_relay(&h, &body), RelayAdmission::Duplicate);
        // Another slot or leader has its own count.
        let h11 = header(FrameKind::Hash, 11, node(1));
        let h2 = header(FrameKind::Hash, 10, node(2));
        assert_eq!(state.admit_relay(&h11, &body), RelayAdmission::Relay);
        assert_eq!(state.admit_relay(&h2, &body), RelayAdmission::Relay);
        // Short bodies do not panic.
        assert_eq!(state.admit_relay(&h11, &[1, 2, 3]), RelayAdmission::Relay);
        assert_eq!(state.admit_relay(&h2, &[]), RelayAdmission::Relay);
        assert_eq!(state.admit_relay(&h2, &[9]), RelayAdmission::OverLimit);
    }

    #[test]
    fn one_relay_at_a_time_per_child() {
        let mut state = TreeState::new();
        assert!(state.start_relay(node(1)));
        assert!(!state.start_relay(node(1)));
        assert!(state.start_relay(node(2)));
        state.finish_relay(&node(1));
        assert!(state.start_relay(node(1)));
        // Finishing what was never started changes nothing.
        state.finish_relay(&node(9));
        state.finish_relay(&node(1));
        state.finish_relay(&node(1));
        assert!(state.start_relay(node(1)));
    }

    #[test]
    fn round_trip_times_keep_their_minimum_and_need_a_matching_probe() {
        let mut state = TreeState::new();
        assert_eq!(state.rtt_of(&node(1)), None);
        for ms in [40u64, 35, 90, 37] {
            state.note_rtt(node(1), Duration::from_millis(ms));
        }
        assert_eq!(state.rtt_of(&node(1)), Some(Duration::from_millis(35)));
        // Old samples leave: after eight longer ones the minimum moves.
        for _ in 0..RTT_SAMPLES {
            state.note_rtt(node(1), Duration::from_millis(60));
        }
        assert_eq!(state.rtt_of(&node(1)), Some(Duration::from_millis(60)));
        state.forget_rtt(&node(1));
        assert_eq!(state.rtt_of(&node(1)), None);

        // An answer counts once, and only to the probe that was sent last.
        assert!(!state.probe_answered(&node(1), 7));
        state.probe_sent(node(1), 7);
        assert!(!state.probe_answered(&node(1), 8));
        assert!(!state.probe_answered(&node(2), 7));
        assert!(state.probe_answered(&node(1), 7));
        assert!(!state.probe_answered(&node(1), 7));
        state.probe_sent(node(1), 9);
        state.probe_sent(node(1), 10);
        assert!(!state.probe_answered(&node(1), 9));
        assert!(state.probe_answered(&node(1), 10));
        // A peer that left has no probe outstanding.
        state.probe_sent(node(1), 11);
        state.forget_rtt(&node(1));
        assert!(!state.probe_answered(&node(1), 11));
        assert!(state.probing());
        state.pause_probing();
        assert!(!state.probing());
    }
}
