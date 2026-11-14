//! Erasure-coded pieces of the leader's tx set (mode `coded`).
//!
//! In a tree a node has one path to the leader's set: its parent. Here the
//! leader cuts the frame it would push into `k` pieces, adds `m - k`
//! Reed-Solomon parity pieces, and every node gets its `m` pieces from `m`
//! different senders. Any `k` of them rebuild the frame, so up to `m - k` of a
//! node's senders can be down, slow or hostile without the node having to
//! wait and ask.
//!
//! * [`coded_params`]: `k`, `m` and the relays per piece, from the node count
//!   alone (every node derives the same).
//! * [`CodedLayout`]: who sends which piece to whom, from the hash order of
//!   the slot's tree (`txset_tree::tree_order`).
//! * [`encode_pieces`], [`rebuild`]: the code.
//! * [`merkle_tree`], [`merkle_root_from`], [`root_message`]: how a piece
//!   proves that it is the leader's. The leader's overlay key signs the root;
//!   a piece carries its path to that root.
//! * [`PieceHeader`]: in front of a piece on the tx-set stream.
//! * [`CodedState`]: the pieces a node holds, and its limits.
//!
//! As with the tree, nothing here is trusted by the user of a set: a rebuilt
//! frame goes down the path every received frame takes (decompress, strict
//! parse, content hash), and Core gets a set only when an SCP statement names
//! that hash. The code protects the delivery, not the set.

use super::txset_tree::NodeId;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io;
use std::sync::Arc;

/// Stands where an ordinary tx-set frame has the big-endian length of its
/// XDR, as `TREE_TAG` does. No length can take this value.
pub const PIECE_TAG: u32 = 0xFFFF_FFF3;

/// Version of the piece frame.
pub const PIECE_VERSION: u8 = 1;

/// Pieces a set is cut into, at most.
pub const MAX_PIECES: usize = 16;

/// With fewer other nodes than this a set is not coded (the leader pushes
/// it to everyone).
pub const MIN_CODED_OTHERS: usize = 4;

/// Roots a node keeps pieces for, per slot and leader. A leader pushes one
/// set per slot it leads, two if it built one early and another at the
/// trigger (as `MAX_RELAYS_PER_SLOT_AND_LEADER` for the tree).
pub const MAX_ROOTS_PER_SLOT_AND_LEADER: usize = 2;

/// Piece frames a node looks at from one sender for one slot and leader. A
/// sender owes it one piece per root; the rest is room for a repeat.
pub const MAX_FRAMES_PER_SENDER: u8 = 6;

/// Pieces queued or in flight to one peer at a time. A peer that stops
/// reading its tx-set stream pins at most this many pieces in the sender's
/// memory; further pieces are not sent to it (the code absorbs the gap).
pub const MAX_PIECES_IN_FLIGHT_PER_PEER: u32 = 4;

/// Roots, senders and relayed pieces a node remembers.
const CODED_MEMORY: usize = 256;

/// The shape of the code for a network.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CodedParams {
    /// Pieces that rebuild the frame (`k`).
    pub data: usize,
    /// Pieces in all (`m`).
    pub pieces: usize,
    /// Relays per piece (`r`).
    pub relays: usize,
}

/// The code for a network with `others` nodes besides the leader: up to
/// [`MAX_PIECES`] pieces, half of them (rounded up) enough to rebuild, and as
/// many relays per piece as the nodes allow with every node relaying one
/// piece at most. `None`: too few nodes to code for.
pub fn coded_params(others: usize) -> Option<CodedParams> {
    if others < MIN_CODED_OTHERS {
        return None;
    }
    let pieces = others.min(MAX_PIECES);
    Some(CodedParams {
        data: pieces.div_ceil(2),
        pieces,
        relays: others / pieces,
    })
}

/// Who sends which piece to whom, for one slot and leader.
///
/// `others` is the hash order of the slot's tree without the leader. Piece
/// `j`'s relays are `others[j*r .. j*r + r]`: the leader sends them piece `j`.
/// The node at position `p` gets piece `j` from relay `(p + j) mod r` of that
/// piece, unless it is one of the piece's relays itself. So the `m` pieces of
/// a node come from `m` different senders, and no piece makes more than two
/// hops.
#[derive(Clone, Debug)]
pub struct CodedLayout {
    pub leader: NodeId,
    pub params: CodedParams,
    others: Vec<NodeId>,
    position: HashMap<NodeId, usize>,
}

impl CodedLayout {
    /// From the order of the slot's tree (`tree_order`: the leader first).
    pub fn new(order: &[NodeId]) -> Option<Self> {
        let (leader, others) = order.split_first()?;
        let params = coded_params(others.len())?;
        Some(Self {
            leader: *leader,
            params,
            others: others.to_vec(),
            position: others.iter().enumerate().map(|(i, n)| (*n, i)).collect(),
        })
    }

    /// The relays of `piece`.
    pub fn relays_of(&self, piece: usize) -> &[NodeId] {
        let r = self.params.relays;
        if piece >= self.params.pieces {
            return &[];
        }
        &self.others[piece * r..piece * r + r]
    }

    /// The piece `node` relays, if any.
    pub fn relayed_piece(&self, node: &NodeId) -> Option<usize> {
        let p = *self.position.get(node)?;
        let piece = p / self.params.relays;
        (piece < self.params.pieces).then_some(piece)
    }

    /// The node that owes `node` the piece `piece`: the leader if `node` is
    /// one of the piece's relays, one of those relays otherwise.
    pub fn sender_of(&self, node: &NodeId, piece: usize) -> Option<NodeId> {
        let p = *self.position.get(node)?;
        if piece >= self.params.pieces {
            return None;
        }
        if self.relayed_piece(node) == Some(piece) {
            return Some(self.leader);
        }
        let relays = self.relays_of(piece);
        Some(relays[(p + piece) % relays.len()])
    }

    /// The nodes `relay` passes its piece on to.
    pub fn receivers_of(&self, relay: &NodeId) -> Vec<NodeId> {
        let Some(piece) = self.relayed_piece(relay) else {
            return Vec::new();
        };
        self.others
            .iter()
            .filter(|node| *node != relay && self.sender_of(node, piece) == Some(*relay))
            .copied()
            .collect()
    }

    /// Every piece the leader writes, with the relay it goes to.
    pub fn leader_targets(&self) -> Vec<(usize, NodeId)> {
        (0..self.params.pieces)
            .flat_map(|piece| self.relays_of(piece).iter().map(move |n| (piece, *n)))
            .collect()
    }

    pub fn contains(&self, node: &NodeId) -> bool {
        self.position.contains_key(node)
    }
}

/// Bytes of one piece of a frame of `total_len` bytes cut into `k`: the
/// frame is padded with zeros to `k` pieces of equal, even length (the code
/// works on 16-bit symbols).
pub fn piece_len(total_len: usize, k: usize) -> usize {
    let len = total_len.div_ceil(k.max(1)).max(2);
    len + len % 2
}

fn invalid(what: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, what)
}

/// Cut `frame` into `params.data` pieces and add the parity pieces: all
/// `params.pieces` of them, the data pieces first.
pub fn encode_pieces(frame: &[u8], params: CodedParams) -> io::Result<Vec<Vec<u8>>> {
    let (k, m) = (params.data, params.pieces);
    if k == 0 || m <= k || frame.is_empty() {
        return Err(invalid("no code for these parameters"));
    }
    let len = piece_len(frame.len(), k);
    let mut pieces: Vec<Vec<u8>> = Vec::with_capacity(m);
    for i in 0..k {
        let start = (i * len).min(frame.len());
        let end = ((i + 1) * len).min(frame.len());
        let mut piece = Vec::with_capacity(len);
        piece.extend_from_slice(&frame[start..end]);
        piece.resize(len, 0);
        pieces.push(piece);
    }
    let parity = reed_solomon_simd::encode(k, m - k, &pieces)
        .map_err(|_| invalid("Reed-Solomon encoding failed"))?;
    pieces.extend(parity);
    Ok(pieces)
}

/// Rebuild a frame of `total_len` bytes from at least `params.data` of its
/// pieces (`pieces[i]` is piece `i`, if held). Returns the frame and whether
/// a parity piece was needed.
pub fn rebuild(
    pieces: &[Option<&[u8]>],
    params: CodedParams,
    total_len: usize,
) -> io::Result<(Vec<u8>, bool)> {
    let (k, m) = (params.data, params.pieces);
    if k == 0 || m <= k || pieces.len() != m {
        return Err(invalid("no code for these parameters"));
    }
    let len = piece_len(total_len, k);
    if total_len == 0 || pieces.iter().flatten().any(|p| p.len() != len) {
        return Err(invalid("piece of the wrong length"));
    }
    let missing = pieces[..k].iter().filter(|p| p.is_none()).count();
    let restored = if missing == 0 {
        None
    } else {
        let originals = pieces[..k]
            .iter()
            .enumerate()
            .filter_map(|(i, p)| p.map(|p| (i, p)));
        let recovery = pieces[k..]
            .iter()
            .enumerate()
            .filter_map(|(i, p)| p.map(|p| (i, p)));
        Some(
            reed_solomon_simd::decode(k, m - k, originals, recovery)
                .map_err(|_| invalid("too few pieces to rebuild"))?,
        )
    };
    let mut frame = Vec::with_capacity(k * len);
    for (i, piece) in pieces[..k].iter().enumerate() {
        match piece {
            Some(piece) => frame.extend_from_slice(piece),
            None => frame.extend_from_slice(
                restored
                    .as_ref()
                    .and_then(|restored| restored.get(&i))
                    .ok_or_else(|| invalid("a piece was not rebuilt"))?,
            ),
        }
    }
    // What was cut off is the padding, and padding is zeros.
    if frame[total_len..].iter().any(|b| *b != 0) {
        return Err(invalid("padding is not zero"));
    }
    frame.truncate(total_len);
    Ok((frame, missing > 0))
}

/// The leaf of piece `index` in the Merkle tree of a set's pieces.
pub fn leaf_hash(index: u8, piece: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update([0u8, index]);
    hasher.update(piece);
    hasher.finalize().into()
}

fn node_hash(left: &[u8; 32], right: &[u8; 32]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update([1u8]);
    hasher.update(left);
    hasher.update(right);
    hasher.finalize().into()
}

/// Levels above the leaves of a tree over `pieces` leaves.
pub fn merkle_depth(pieces: usize) -> usize {
    pieces.max(1).next_power_of_two().trailing_zeros() as usize
}

/// The root of the tree over `leaves` (padded with zero leaves to a power of
/// two) and, for every leaf, its path to the root (the sibling at each
/// level, bottom up).
pub fn merkle_tree(leaves: &[[u8; 32]]) -> ([u8; 32], Vec<Vec<[u8; 32]>>) {
    let width = leaves.len().max(1).next_power_of_two();
    let mut level: Vec<[u8; 32]> = leaves.to_vec();
    level.resize(width, [0u8; 32]);
    let mut paths: Vec<Vec<[u8; 32]>> = vec![Vec::new(); leaves.len()];
    let mut span = 1;
    while level.len() > 1 {
        for (leaf, path) in paths.iter_mut().enumerate() {
            path.push(level[(leaf / span) ^ 1]);
        }
        level = level
            .chunks_exact(2)
            .map(|pair| node_hash(&pair[0], &pair[1]))
            .collect();
        span *= 2;
    }
    (level[0], paths)
}

/// The root a leaf at `index` reaches along `path`.
pub fn merkle_root_from(leaf: [u8; 32], index: usize, path: &[[u8; 32]]) -> [u8; 32] {
    let mut at = index;
    let mut hash = leaf;
    for sibling in path {
        hash = if at % 2 == 0 {
            node_hash(&hash, sibling)
        } else {
            node_hash(sibling, &hash)
        };
        at /= 2;
    }
    hash
}

/// What the leader's overlay key signs for a set it cut into pieces: the
/// network, the slot, the leader, the shape of the code, the frame's length
/// and the root of its pieces.
pub fn root_message(
    network_id: &[u8; 32],
    slot: u32,
    leader: &NodeId,
    params: CodedParams,
    total_len: u32,
    root: &[u8; 32],
) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(b"stellar-overlay tx set pieces v1");
    hasher.update(network_id);
    hasher.update(slot.to_be_bytes());
    hasher.update(leader);
    hasher.update([params.data as u8, params.pieces as u8]);
    hasher.update(total_len.to_be_bytes());
    hasher.update(root);
    hasher.finalize().into()
}

/// Bytes of a piece header without its Merkle path.
pub const PIECE_HEADER_FIXED: usize = 4 + 8 + 4 + 32 + 8 + 4 + 32 + 32 + 64 + 64;

/// The largest piece header.
pub const MAX_PIECE_HEADER: usize = PIECE_HEADER_FIXED + 32 * 8;

/// In front of a piece.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PieceHeader {
    /// 1 = straight from the leader, 2 = through a relay.
    pub hop: u8,
    /// `k` and `m` of the code.
    pub data: u8,
    pub pieces: u8,
    /// Which piece this is.
    pub index: u8,
    pub slot: u32,
    pub leader: NodeId,
    /// When the leader sent the set, in Unix milliseconds by its clock.
    pub sent_unix_ms: u64,
    /// Bytes of the frame the pieces rebuild.
    pub total_len: u32,
    /// Root of the Merkle tree over the set's pieces.
    pub root: [u8; 32],
    /// The Ed25519 key of the leader's overlay.
    pub overlay_key: [u8; 32],
    /// The leader's (validator's) signature that binds that overlay to it:
    /// what nodes exchange as their identity (`node_id_binding`).
    pub binding_sig: [u8; 64],
    /// The overlay key's signature over [`root_message`].
    pub root_sig: [u8; 64],
    /// The piece's path to the root.
    pub path: Vec<[u8; 32]>,
}

impl PieceHeader {
    pub fn params(&self) -> (usize, usize) {
        (self.data as usize, self.pieces as usize)
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(PIECE_HEADER_FIXED + 32 * self.path.len());
        out.extend_from_slice(&PIECE_TAG.to_be_bytes());
        out.push(PIECE_VERSION);
        out.push(self.hop);
        out.push(self.data);
        out.push(self.pieces);
        out.push(self.index);
        out.push(self.path.len() as u8);
        out.extend_from_slice(&[0, 0]);
        out.extend_from_slice(&self.slot.to_be_bytes());
        out.extend_from_slice(&self.leader);
        out.extend_from_slice(&self.sent_unix_ms.to_be_bytes());
        out.extend_from_slice(&self.total_len.to_be_bytes());
        out.extend_from_slice(&self.root);
        out.extend_from_slice(&self.overlay_key);
        out.extend_from_slice(&self.binding_sig);
        out.extend_from_slice(&self.root_sig);
        for hash in &self.path {
            out.extend_from_slice(hash);
        }
        out
    }

    /// Whether a frame starts with the piece tag.
    pub fn is_tagged(frame: &[u8]) -> bool {
        frame.len() >= 4 && frame[..4] == PIECE_TAG.to_be_bytes()
    }

    /// Parse the header of a piece frame. Returns it with the offset at
    /// which the piece begins, or `None` if the frame is malformed: a field
    /// out of range, a path of the wrong length for `m`, or a piece that
    /// does not have the length the frame's length and `k` give.
    pub fn parse(frame: &[u8]) -> Option<(Self, usize)> {
        if !Self::is_tagged(frame) || frame.len() < PIECE_HEADER_FIXED {
            return None;
        }
        if frame[4] != PIECE_VERSION || frame[10] != 0 || frame[11] != 0 {
            return None;
        }
        let (hop, data, pieces, index, path_len) =
            (frame[5], frame[6], frame[7], frame[8], frame[9] as usize);
        if !(1..=2).contains(&hop)
            || data == 0
            || pieces <= data
            || pieces as usize > MAX_PIECES
            || index >= pieces
            || path_len != merkle_depth(pieces as usize)
        {
            return None;
        }
        let take32 = |at: usize| -> [u8; 32] { frame[at..at + 32].try_into().unwrap() };
        let take64 = |at: usize| -> [u8; 64] { frame[at..at + 64].try_into().unwrap() };
        let slot = u32::from_be_bytes(frame[12..16].try_into().unwrap());
        let leader = take32(16);
        let sent_unix_ms = u64::from_be_bytes(frame[48..56].try_into().unwrap());
        let total_len = u32::from_be_bytes(frame[56..60].try_into().unwrap());
        let root = take32(60);
        let overlay_key = take32(92);
        let binding_sig = take64(124);
        let root_sig = take64(188);
        let body = PIECE_HEADER_FIXED + 32 * path_len;
        if frame.len() < body
            || (total_len as usize) < 4
            || total_len as usize > super::txset_encoding::max_encoded_size()
            || frame.len() - body != piece_len(total_len as usize, data as usize)
        {
            return None;
        }
        let path = (0..path_len)
            .map(|i| take32(PIECE_HEADER_FIXED + 32 * i))
            .collect();
        Some((
            Self {
                hop,
                data,
                pieces,
                index,
                slot,
                leader,
                sent_unix_ms,
                total_len,
                root,
                overlay_key,
                binding_sig,
                root_sig,
                path,
            },
            body,
        ))
    }
}

/// A piece as it arrived: the frame, and where the piece begins in it.
#[derive(Clone, Debug)]
pub struct PieceBytes {
    pub frame: Arc<Vec<u8>>,
    pub at: usize,
}

impl PieceBytes {
    pub fn as_slice(&self) -> &[u8] {
        &self.frame[self.at..]
    }
}

/// A set's pieces are told apart by slot, leader and root.
pub type RootKey = (u32, NodeId, [u8; 32]);

struct Assembly {
    data: usize,
    total_len: u32,
    pieces: Vec<Option<PieceBytes>>,
    /// Indices seen, also after the pieces were handed out.
    seen: Vec<bool>,
    done: bool,
}

/// Enough pieces of a set to rebuild it.
#[derive(Debug)]
pub struct ReadySet {
    pub pieces: Vec<Option<PieceBytes>>,
    pub params: CodedParams,
    pub total_len: u32,
}

/// What became of a valid piece.
#[derive(Debug)]
pub enum PieceOutcome {
    /// Kept; more are needed.
    Stored,
    /// With this one the set can be rebuilt.
    Ready(ReadySet),
    /// This index is held already.
    Duplicate,
    /// The set was rebuilt before this piece came.
    Late,
    /// The root is not one this node keeps pieces for (see
    /// [`CodedState::add_root`]), or the piece does not fit it.
    Unknown,
}

/// The pieces a node holds and the limits on what peers can make it hold.
pub struct CodedState {
    assemblies: lru::LruCache<RootKey, Assembly>,
    /// Roots kept per slot and leader.
    roots: lru::LruCache<(u32, NodeId), Vec<[u8; 32]>>,
    /// Piece frames looked at per slot, leader and sender.
    frames: lru::LruCache<(u32, NodeId, NodeId), u8>,
    /// Roots whose piece this node passed on as a relay.
    relayed: lru::LruCache<RootKey, ()>,
    /// Sets this node cut into pieces as their leader, by content hash.
    pushed: lru::LruCache<[u8; 32], ()>,
    /// Pieces not yet handed to the transport, per receiver.
    in_flight: HashMap<NodeId, u32>,
}

impl Default for CodedState {
    fn default() -> Self {
        Self::new()
    }
}

impl CodedState {
    pub fn new() -> Self {
        let memory = std::num::NonZeroUsize::new(CODED_MEMORY).unwrap();
        Self {
            assemblies: lru::LruCache::new(memory),
            roots: lru::LruCache::new(memory),
            frames: lru::LruCache::new(std::num::NonZeroUsize::new(16 * CODED_MEMORY).unwrap()),
            relayed: lru::LruCache::new(memory),
            pushed: lru::LruCache::new(memory),
            in_flight: HashMap::new(),
        }
    }

    /// A piece frame for `slot` and `leader` came from `sender`. False: that
    /// sender used up what this node looks at for that slot and leader.
    pub fn admit_frame(&mut self, slot: u32, leader: NodeId, sender: NodeId) -> bool {
        let count = self.frames.get_or_insert_mut((slot, leader, sender), || 0);
        if *count >= MAX_FRAMES_PER_SENDER {
            return false;
        }
        *count += 1;
        true
    }

    /// Whether pieces are kept for this root with this shape: `Some(true)`.
    /// `Some(false)`: the root is known with another shape or length (a
    /// frame that says so is wrong). `None`: not known; its signature has to
    /// be checked before [`Self::add_root`].
    pub fn knows_root(&self, key: &RootKey, params: CodedParams, total_len: u32) -> Option<bool> {
        self.assemblies.peek(key).map(|a| {
            // (`seen`, not `pieces`: the pieces leave with the rebuild.)
            a.data == params.data && a.seen.len() == params.pieces && a.total_len == total_len
        })
    }

    /// Start keeping pieces for a root whose signature by the slot's leader
    /// was checked. False: the leader has [`MAX_ROOTS_PER_SLOT_AND_LEADER`]
    /// other roots for that slot already.
    pub fn add_root(&mut self, key: RootKey, params: CodedParams, total_len: u32) -> bool {
        if self.assemblies.contains(&key) {
            return true;
        }
        let roots = self.roots.get_or_insert_mut((key.0, key.1), Vec::new);
        if roots.len() >= MAX_ROOTS_PER_SLOT_AND_LEADER {
            return false;
        }
        roots.push(key.2);
        self.assemblies.put(
            key,
            Assembly {
                data: params.data,
                total_len,
                pieces: vec![None; params.pieces],
                seen: vec![false; params.pieces],
                done: false,
            },
        );
        true
    }

    /// Keep a piece whose path to the root was checked.
    pub fn add_piece(&mut self, key: &RootKey, index: usize, piece: PieceBytes) -> PieceOutcome {
        let Some(assembly) = self.assemblies.get_mut(key) else {
            return PieceOutcome::Unknown;
        };
        if index >= assembly.seen.len() {
            return PieceOutcome::Unknown;
        }
        if std::mem::replace(&mut assembly.seen[index], true) {
            return PieceOutcome::Duplicate;
        }
        if assembly.done {
            return PieceOutcome::Late;
        }
        assembly.pieces[index] = Some(piece);
        let have = assembly.pieces.iter().flatten().count();
        if have < assembly.data {
            return PieceOutcome::Stored;
        }
        assembly.done = true;
        let pieces = std::mem::take(&mut assembly.pieces);
        PieceOutcome::Ready(ReadySet {
            params: CodedParams {
                data: assembly.data,
                pieces: assembly.seen.len(),
                relays: 0,
            },
            total_len: assembly.total_len,
            pieces,
        })
    }

    /// This node is about to pass on its piece of this root. False: it did
    /// so already.
    pub fn mark_relayed(&mut self, key: RootKey) -> bool {
        self.relayed.put(key, ()).is_none()
    }

    /// This node is about to cut the set `hash` into pieces as its leader.
    /// False: it did so already.
    pub fn mark_pushed(&mut self, hash: [u8; 32]) -> bool {
        self.pushed.put(hash, ()).is_none()
    }

    /// A piece is about to be written to `node`. False: too many are in
    /// flight to it, and this one is not sent.
    pub fn start_send(&mut self, node: NodeId) -> bool {
        let in_flight = self.in_flight.entry(node).or_insert(0);
        if *in_flight >= MAX_PIECES_IN_FLIGHT_PER_PEER {
            return false;
        }
        *in_flight += 1;
        true
    }

    /// The piece to `node` was handed to the transport, or failed.
    pub fn finish_send(&mut self, node: &NodeId) {
        if let Some(count) = self.in_flight.get_mut(node) {
            *count = count.saturating_sub(1);
            if *count == 0 {
                self.in_flight.remove(node);
            }
        }
    }

    /// Drop the pieces of slots below `min_slot` (their sets are decided).
    pub fn prune(&mut self, min_slot: u32) {
        let old: Vec<RootKey> = self
            .assemblies
            .iter()
            .filter(|(key, _)| key.0 < min_slot)
            .map(|(key, _)| *key)
            .collect();
        for key in old {
            self.assemblies.pop(&key);
        }
        let old: Vec<(u32, NodeId)> = self
            .roots
            .iter()
            .filter(|(key, _)| key.0 < min_slot)
            .map(|(key, _)| *key)
            .collect();
        for key in old {
            self.roots.pop(&key);
        }
    }

    /// Bytes of pieces held (for tests).
    #[cfg(test)]
    pub fn held_bytes(&self) -> usize {
        self.assemblies
            .iter()
            .flat_map(|(_, a)| a.pieces.iter().flatten())
            .map(|p| p.as_slice().len())
            .sum()
    }
}

#[cfg(test)]
mod tests {
    use super::super::txset_tree::tree_order;
    use super::*;
    use rand::{RngCore, SeedableRng};

    fn node(n: u16) -> NodeId {
        let mut id = [0u8; 32];
        id[..2].copy_from_slice(&n.to_be_bytes());
        id[31] = 0xAA;
        id
    }

    fn nodes(count: u16) -> Vec<NodeId> {
        (0..count).map(node).collect()
    }

    fn params(data: usize, pieces: usize) -> CodedParams {
        CodedParams {
            data,
            pieces,
            relays: 1,
        }
    }

    #[test]
    fn the_code_is_derived_from_the_node_count() {
        for (others, expected) in [
            (0, None),
            (3, None),
            (4, Some((2, 4, 1))),
            (6, Some((3, 6, 1))),
            (7, Some((4, 7, 1))),
            (16, Some((8, 16, 1))),
            (17, Some((8, 16, 1))),
            (31, Some((8, 16, 1))),
            (32, Some((8, 16, 2))),
            (89, Some((8, 16, 5))),
            (1023, Some((8, 16, 63))),
        ] {
            let got = coded_params(others).map(|p| (p.data, p.pieces, p.relays));
            assert_eq!(got, expected, "{others} others");
            if let Some((k, m, r)) = got {
                assert!(k < m && m * r <= others);
            }
        }
    }

    #[test]
    fn every_node_gets_every_piece_from_another_sender() {
        for count in [5u16, 7, 17, 33, 90, 200] {
            let all = nodes(count);
            let order = tree_order(&[3u8; 32], &all[2], &all);
            let layout = CodedLayout::new(&order).unwrap();
            let p = layout.params;
            // The relay sets of the pieces are disjoint, and the leader
            // writes each piece to its relays.
            let targets = layout.leader_targets();
            assert_eq!(targets.len(), p.pieces * p.relays);
            let mut relays: Vec<NodeId> = targets.iter().map(|(_, n)| *n).collect();
            relays.sort();
            relays.dedup();
            assert_eq!(relays.len(), p.pieces * p.relays);
            assert!(!relays.contains(&all[2]));
            for me in order.iter().skip(1) {
                let senders: Vec<NodeId> = (0..p.pieces)
                    .map(|j| layout.sender_of(me, j).unwrap())
                    .collect();
                // m different senders, never the node itself, the leader
                // for the one piece the node relays (if it relays one).
                let mut distinct = senders.clone();
                distinct.sort();
                distinct.dedup();
                assert_eq!(distinct.len(), p.pieces, "{count} nodes");
                assert!(!senders.contains(me));
                let from_leader = senders.iter().filter(|s| **s == all[2]).count();
                assert_eq!(from_leader, usize::from(layout.relayed_piece(me).is_some()));
                // A relay sends exactly to the nodes that expect it to.
                for (j, sender) in senders.iter().enumerate() {
                    if *sender != all[2] {
                        assert_eq!(layout.relayed_piece(sender), Some(j));
                        assert!(layout.receivers_of(sender).contains(me));
                    }
                }
            }
            // The receivers of a piece's relays are everyone but its relays,
            // each once; the load of a relay is even within one.
            for j in 0..p.pieces {
                let mut got: Vec<NodeId> = Vec::new();
                let mut loads = Vec::new();
                for relay in layout.relays_of(j) {
                    let receivers = layout.receivers_of(relay);
                    loads.push(receivers.len());
                    got.extend(receivers);
                }
                got.sort();
                let before = got.len();
                got.dedup();
                assert_eq!(got.len(), before);
                assert_eq!(got.len(), count as usize - 1 - p.relays);
                assert!(loads.iter().max().unwrap() - loads.iter().min().unwrap() <= 2);
            }
            // A node outside the layout is owed nothing.
            assert_eq!(layout.sender_of(&node(9999), 0), None);
            assert!(layout.receivers_of(&node(9999)).is_empty());
            assert!(layout.receivers_of(&all[2]).is_empty());
        }
        // Too few nodes: no layout.
        let few = nodes(4);
        assert!(CodedLayout::new(&tree_order(&[3u8; 32], &few[0], &few)).is_none());
    }

    #[test]
    fn any_k_pieces_rebuild_the_frame() {
        let mut rng = rand::rngs::StdRng::seed_from_u64(11);
        for (len, k, m) in [
            (5usize, 2usize, 4usize),
            (4, 3, 6),
            (1, 4, 7),
            (1000, 3, 6),
            (1001, 8, 16),
            (123_457, 8, 16),
            (1_000_003, 8, 16),
        ] {
            let mut frame = vec![0u8; len];
            rng.fill_bytes(&mut frame);
            let p = params(k, m);
            let pieces = encode_pieces(&frame, p).unwrap();
            assert_eq!(pieces.len(), m);
            let piece = piece_len(len, k);
            assert!(piece % 2 == 0 && piece * k >= len && piece * k < len + 2 * k + 2);
            assert!(pieces.iter().all(|p| p.len() == piece));
            // The data pieces alone: no parity needed.
            let held: Vec<Option<&[u8]>> = (0..m)
                .map(|i| (i < k).then(|| pieces[i].as_slice()))
                .collect();
            assert_eq!(rebuild(&held, p, len).unwrap(), (frame.clone(), false));
            // The parity pieces and as few data pieces as that leaves.
            let held: Vec<Option<&[u8]>> = (0..m)
                .map(|i| (i >= m - k).then(|| pieces[i].as_slice()))
                .collect();
            assert_eq!(rebuild(&held, p, len).unwrap(), (frame.clone(), true));
            // Random subsets of exactly k pieces.
            for _ in 0..8 {
                let mut idx: Vec<usize> = (0..m).collect();
                for i in (1..m).rev() {
                    idx.swap(i, (rng.next_u32() as usize) % (i + 1));
                }
                let chosen = &idx[..k];
                let held: Vec<Option<&[u8]>> = (0..m)
                    .map(|i| chosen.contains(&i).then(|| pieces[i].as_slice()))
                    .collect();
                assert_eq!(rebuild(&held, p, len).unwrap().0, frame);
            }
            // One piece too few.
            let held: Vec<Option<&[u8]>> = (0..m)
                .map(|i| (i >= m - k + 1).then(|| pieces[i].as_slice()))
                .collect();
            assert!(rebuild(&held, p, len).is_err());
            // A piece of another length.
            let short = &pieces[0][..piece - 2];
            let mut held: Vec<Option<&[u8]>> = (0..m)
                .map(|i| (i < k).then(|| pieces[i].as_slice()))
                .collect();
            held[0] = Some(short);
            assert!(rebuild(&held, p, len).is_err());
        }
        assert!(encode_pieces(&[], params(2, 4)).is_err());
        assert!(encode_pieces(&[1, 2, 3], params(4, 4)).is_err());
    }

    #[test]
    fn a_wrong_piece_gives_a_wrong_frame_and_the_path_tells() {
        let mut rng = rand::rngs::StdRng::seed_from_u64(5);
        let mut frame = vec![0u8; 40_000];
        rng.fill_bytes(&mut frame);
        let p = params(8, 16);
        let pieces = encode_pieces(&frame, p).unwrap();
        let leaves: Vec<[u8; 32]> = pieces
            .iter()
            .enumerate()
            .map(|(i, piece)| leaf_hash(i as u8, piece))
            .collect();
        let (root, paths) = merkle_tree(&leaves);
        assert_eq!(paths.len(), 16);
        assert!(paths.iter().all(|path| path.len() == merkle_depth(16)));
        for (i, piece) in pieces.iter().enumerate() {
            assert_eq!(
                merkle_root_from(leaf_hash(i as u8, piece), i, &paths[i]),
                root
            );
            // The same bytes at another index, or another path, do not
            // reach the root.
            let other = (i + 1) % 16;
            assert_ne!(
                merkle_root_from(leaf_hash(other as u8, piece), other, &paths[other]),
                root
            );
            assert_ne!(
                merkle_root_from(leaf_hash(i as u8, piece), i, &paths[other]),
                root
            );
        }
        // A changed parity piece: the rebuild that uses it gives another
        // frame (or none), and its path no longer reaches the root.
        let mut bad = pieces[9].clone();
        bad[100] ^= 1;
        assert_ne!(merkle_root_from(leaf_hash(9, &bad), 9, &paths[9]), root);
        let held: Vec<Option<&[u8]>> = (0..16)
            .map(|i| match i {
                9 => Some(bad.as_slice()),
                1..=7 => Some(pieces[i].as_slice()),
                _ => None,
            })
            .collect();
        match rebuild(&held, p, frame.len()) {
            Ok((rebuilt, _)) => assert_ne!(rebuilt, frame),
            Err(_) => {}
        }
    }

    #[test]
    fn merkle_trees_of_other_sizes() {
        for count in [2usize, 3, 4, 5, 6, 7, 9, 16] {
            let leaves: Vec<[u8; 32]> = (0..count)
                .map(|i| leaf_hash(i as u8, &[i as u8; 10]))
                .collect();
            let (root, paths) = merkle_tree(&leaves);
            for i in 0..count {
                assert_eq!(paths[i].len(), merkle_depth(count));
                assert_eq!(merkle_root_from(leaves[i], i, &paths[i]), root);
            }
            // A padding leaf is not a piece: no bytes hash to it.
            if count < count.next_power_of_two() {
                assert_ne!(leaf_hash(count as u8, &[]), [0u8; 32]);
            }
        }
        assert_eq!(merkle_depth(2), 1);
        assert_eq!(merkle_depth(6), 3);
        assert_eq!(merkle_depth(16), 4);
    }

    fn header(index: u8, total_len: u32) -> PieceHeader {
        PieceHeader {
            hop: 1,
            data: 8,
            pieces: 16,
            index,
            slot: 77,
            leader: node(3),
            sent_unix_ms: 1_700_000_000_123,
            total_len,
            root: [9u8; 32],
            overlay_key: [8u8; 32],
            binding_sig: [7u8; 64],
            root_sig: [6u8; 64],
            path: vec![[1u8; 32], [2u8; 32], [3u8; 32], [4u8; 32]],
        }
    }

    #[test]
    fn a_piece_header_round_trips_and_malformed_ones_are_refused() {
        let h = header(5, 1000);
        let piece = vec![0x55u8; piece_len(1000, 8)];
        let mut frame = h.encode();
        assert_eq!(frame.len(), PIECE_HEADER_FIXED + 4 * 32);
        frame.extend_from_slice(&piece);
        assert!(PieceHeader::is_tagged(&frame));
        let (parsed, at) = PieceHeader::parse(&frame).unwrap();
        assert_eq!(parsed, h);
        assert_eq!(&frame[at..], piece.as_slice());
        // Not a tree frame and not a length.
        assert!(!crate::flood::txset_tree::TreeHeader::is_tagged(&frame));
        assert!(PIECE_TAG as usize > super::super::txset_encoding::MAX_TXSET_XDR_SIZE);

        let broken = |change: &dyn Fn(&mut Vec<u8>)| {
            let mut f = frame.clone();
            change(&mut f);
            PieceHeader::parse(&f).is_none()
        };
        assert!(broken(&|f| f[4] = 2)); // version
        assert!(broken(&|f| f[5] = 0)); // hop
        assert!(broken(&|f| f[5] = 3));
        assert!(broken(&|f| f[6] = 0)); // k
        assert!(broken(&|f| f[6] = 16)); // k = m
        assert!(broken(&|f| f[7] = 17)); // m over the limit
        assert!(broken(&|f| f[8] = 16)); // index = m
        assert!(broken(&|f| f[9] = 3)); // path length
        assert!(broken(&|f| f[10] = 1)); // reserved
        assert!(broken(&|f| f.push(0))); // piece too long
        assert!(broken(&|f| {
            f.pop();
        }));
        assert!(broken(&|f| f[56..60].copy_from_slice(&0u32.to_be_bytes())));
        assert!(broken(
            &|f| f[56..60].copy_from_slice(&u32::MAX.to_be_bytes())
        ));
        assert!(broken(&|f| f.truncate(100)));
        assert!(PieceHeader::parse(&frame[..PIECE_HEADER_FIXED]).is_none());
    }

    fn piece_bytes(len: usize, fill: u8) -> PieceBytes {
        PieceBytes {
            frame: Arc::new(vec![fill; len + 3]),
            at: 3,
        }
    }

    #[test]
    fn a_node_keeps_pieces_until_it_can_rebuild() {
        let mut state = CodedState::new();
        let p = params(3, 6);
        let key: RootKey = (10, node(1), [1u8; 32]);
        // Nothing is kept for a root nobody vouched for.
        assert!(matches!(
            state.add_piece(&key, 0, piece_bytes(8, 0)),
            PieceOutcome::Unknown
        ));
        assert_eq!(state.knows_root(&key, p, 20), None);
        assert!(state.add_root(key, p, 20));
        assert_eq!(state.knows_root(&key, p, 20), Some(true));
        assert_eq!(state.knows_root(&key, p, 21), Some(false));
        assert_eq!(state.knows_root(&key, params(2, 6), 20), Some(false));
        assert!(matches!(
            state.add_piece(&key, 4, piece_bytes(8, 4)),
            PieceOutcome::Stored
        ));
        assert!(matches!(
            state.add_piece(&key, 4, piece_bytes(8, 4)),
            PieceOutcome::Duplicate
        ));
        assert!(matches!(
            state.add_piece(&key, 6, piece_bytes(8, 6)),
            PieceOutcome::Unknown
        ));
        assert!(matches!(
            state.add_piece(&key, 0, piece_bytes(8, 0)),
            PieceOutcome::Stored
        ));
        assert_eq!(state.held_bytes(), 16);
        match state.add_piece(&key, 5, piece_bytes(8, 5)) {
            PieceOutcome::Ready(ready) => {
                assert_eq!(
                    (ready.params.data, ready.params.pieces, ready.total_len),
                    (3, 6, 20)
                );
                let held: Vec<usize> = ready
                    .pieces
                    .iter()
                    .enumerate()
                    .filter_map(|(i, p)| p.as_ref().map(|_| i))
                    .collect();
                assert_eq!(held, vec![0, 4, 5]);
                assert_eq!(ready.pieces[4].as_ref().unwrap().as_slice(), &[4u8; 8]);
            }
            other => panic!("{other:?}"),
        }
        // The pieces left with the rebuild; later ones are not kept.
        assert_eq!(state.held_bytes(), 0);
        assert_eq!(state.knows_root(&key, p, 20), Some(true));
        assert!(matches!(
            state.add_piece(&key, 1, piece_bytes(8, 1)),
            PieceOutcome::Late
        ));
        assert!(matches!(
            state.add_piece(&key, 1, piece_bytes(8, 1)),
            PieceOutcome::Duplicate
        ));
        assert!(matches!(
            state.add_piece(&key, 0, piece_bytes(8, 0)),
            PieceOutcome::Duplicate
        ));
        assert_eq!(state.held_bytes(), 0);
    }

    #[test]
    fn the_limits_of_what_peers_can_make_a_node_hold() {
        let mut state = CodedState::new();
        let p = params(3, 6);
        // Two roots per slot and leader.
        assert!(state.add_root((10, node(1), [1u8; 32]), p, 20));
        assert!(state.add_root((10, node(1), [2u8; 32]), p, 20));
        assert!(state.add_root((10, node(1), [1u8; 32]), p, 20));
        assert!(!state.add_root((10, node(1), [3u8; 32]), p, 20));
        assert_eq!(state.knows_root(&(10, node(1), [3u8; 32]), p, 20), None);
        // Another leader or slot has its own.
        assert!(state.add_root((10, node(2), [3u8; 32]), p, 20));
        assert!(state.add_root((11, node(1), [3u8; 32]), p, 20));
        // A sender is listened to a few times per slot and leader.
        for _ in 0..MAX_FRAMES_PER_SENDER {
            assert!(state.admit_frame(10, node(1), node(7)));
        }
        assert!(!state.admit_frame(10, node(1), node(7)));
        assert!(state.admit_frame(10, node(1), node(8)));
        assert!(state.admit_frame(11, node(1), node(7)));
        // Pieces in flight per receiver.
        for _ in 0..MAX_PIECES_IN_FLIGHT_PER_PEER {
            assert!(state.start_send(node(5)));
        }
        assert!(!state.start_send(node(5)));
        assert!(state.start_send(node(6)));
        state.finish_send(&node(5));
        assert!(state.start_send(node(5)));
        // A relay passes a root's piece on once; a leader cuts a set once.
        assert!(state.mark_relayed((10, node(1), [1u8; 32])));
        assert!(!state.mark_relayed((10, node(1), [1u8; 32])));
        assert!(state.mark_pushed([4u8; 32]));
        assert!(!state.mark_pushed([4u8; 32]));
        // Old slots are dropped with their pieces, and their roots' places.
        assert!(matches!(
            state.add_piece(&(10, node(1), [1u8; 32]), 0, piece_bytes(8, 0)),
            PieceOutcome::Stored
        ));
        assert!(matches!(
            state.add_piece(&(11, node(1), [3u8; 32]), 0, piece_bytes(8, 0)),
            PieceOutcome::Stored
        ));
        assert_eq!(state.held_bytes(), 16);
        state.prune(11);
        assert_eq!(state.held_bytes(), 8);
        assert_eq!(state.knows_root(&(10, node(1), [1u8; 32]), p, 20), None);
        assert_eq!(
            state.knows_root(&(11, node(1), [3u8; 32]), p, 20),
            Some(true)
        );
    }

    #[test]
    fn the_signed_message_binds_every_field() {
        let p = params(8, 16);
        let base = root_message(&[1u8; 32], 5, &node(1), p, 1000, &[2u8; 32]);
        assert_ne!(
            base,
            root_message(&[9u8; 32], 5, &node(1), p, 1000, &[2u8; 32])
        );
        assert_ne!(
            base,
            root_message(&[1u8; 32], 6, &node(1), p, 1000, &[2u8; 32])
        );
        assert_ne!(
            base,
            root_message(&[1u8; 32], 5, &node(2), p, 1000, &[2u8; 32])
        );
        assert_ne!(
            base,
            root_message(&[1u8; 32], 5, &node(1), params(7, 16), 1000, &[2u8; 32])
        );
        assert_ne!(
            base,
            root_message(&[1u8; 32], 5, &node(1), params(8, 15), 1000, &[2u8; 32])
        );
        assert_ne!(
            base,
            root_message(&[1u8; 32], 5, &node(1), p, 1001, &[2u8; 32])
        );
        assert_ne!(
            base,
            root_message(&[1u8; 32], 5, &node(1), p, 1000, &[3u8; 32])
        );
        // The relays per piece are not part of the code.
        let other = CodedParams { relays: 5, ..p };
        assert_eq!(
            base,
            root_message(&[1u8; 32], 5, &node(1), other, 1000, &[2u8; 32])
        );
    }
}
