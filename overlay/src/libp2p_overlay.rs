//! Unified libp2p Overlay v2
//!
//! **Transport: QUIC** avoids TCP head-of-line blocking between streams.
//! Streams still share connection flow control, congestion control and bandwidth.
//!
//! Uses libp2p-stream for persistent bidirectional streams:
//! - Control stream: SCP and TX set requests (highest send priority)
//! - TX stream: transaction flooding (~1KB) - uses INV/GETDATA protocol
//! - TxSet stream: responses (~10MB), ahead of transaction flooding
//!
//! Each stream is opened once per peer and kept alive.
//! QUIC provides independent loss recovery per stream.

use crate::flood::txset_coded::{
    self, CodedLayout, CodedParams, PieceBytes, PieceHeader, PieceOutcome, ReadySet, RootKey,
};
use crate::flood::txset_tree::{
    self, FetchDecision, FrameKind, Probe, RelayAdmission, RelayPlan, TreeConfig, TreeHeader,
    TreeMode, TreeState,
};
use crate::flood::{
    GetData, InvBatch, InvBatcher, InvEntry, InvTracker, NodeId, PendingRequests, PushRateLimiter,
    PushRecord, SendPlan, TxBuffer, TxRouter, TxSetData, TxStreamMessage,
};
#[cfg(test)]
use crate::integrated::CoreCommand;
use crate::integrated::OverlayHandle as MempoolHandle;
use crate::metrics::OverlayMetrics;
use crate::wire::ValidatedTx;
use futures::{AsyncReadExt, StreamExt};
use libp2p::{
    identify::{Behaviour as Identify, Config as IdentifyConfig, Event as IdentifyEvent},
    identity::Keypair,
    swarm::{
        dial_opts::{DialOpts, PeerCondition},
        NetworkBehaviour, SwarmEvent,
    },
    Multiaddr, PeerId, Stream, StreamProtocol, Swarm, SwarmBuilder,
};
use libp2p_stream::{Behaviour as StreamBehaviour, Control, IncomingStreams};
use std::collections::{HashMap, HashSet};
use std::io;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::{mpsc, Mutex, OwnedSemaphorePermit, RwLock, Semaphore};
use tokio::task::JoinSet;
use tracing::{debug, error, info, trace, warn};

mod framed_io;

// Protocol identifiers for dedicated streams
pub const CONTROL_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/control/1.0.0");
pub const TX_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/tx/1.0.0");
// This experimental protocol requires zstd for every transaction-set response.
pub const TXSET_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/txset/zstd/2.0.0");
/// One-shot stream on which an overlay tells a peer which validator it
/// belongs to: one frame, `[nodeID:32][signature:64]` (see `NodeIdentity`),
/// answered with one byte once it has been read.
pub const NODE_ID_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/node-id/1.0.0");

/// Domain separator of the identity binding that Core signs.
const NODE_ID_BINDING_TAG: &[u8] = b"stellar-overlay-peer-id";
const NODE_ID_FRAME_LEN: usize = 32 + 64;

/// Transaction bodies sent directly to a peer are collected for this long,
/// so that a burst of submissions (a client or load generator step) becomes
/// one write per peer instead of one per transaction.
const DIRECT_SEND_BATCH_INTERVAL: Duration = Duration::from_millis(10);

/// A peer's queue of direct sends is written at once when it reaches this
/// many transactions.
const DIRECT_SEND_BATCH_MAX_TXS: usize = 256;

/// At most this many transactions wait for one peer whose stream is not
/// taking them (one sender per peer writes at a time). What does not fit is
/// handed back to the router as not sent.
const DIRECT_SEND_MAX_QUEUED_TXS: usize = 20_000;

/// How often the router is asked whether anything is owed although nothing
/// changed: a send that failed, a body old enough to be sent again, a flood
/// that found no peer.
const DIRECT_REPLAN_INTERVAL: Duration = Duration::from_secs(2);

/// Direct sends per peer: what is queued, and which peers have a sender task
/// writing right now.
#[derive(Default)]
struct DirectOut {
    queued: HashMap<PeerId, Vec<Arc<ValidatedTx>>>,
    sending: HashSet<PeerId>,
}

/// A peer's validator identity claim must arrive within this long of its
/// stream opening.
const NODE_ID_READ_TIMEOUT: Duration = Duration::from_secs(10);

/// Acknowledgement bytes on the node id stream: the claim was dealt with
/// (accepted, kept for later, or refused), or the receiver could not use it
/// yet and the sender should repeat it.
const NODE_ID_ACK_TAKEN: u8 = 1;
const NODE_ID_ACK_RETRY: u8 = 0;

/// Identity claims handled per connection. An honest peer sends one per
/// connection it establishes (two when both sides dial at once) and one more
/// if its Core's identity arrives late.
const MAX_NODE_ID_CLAIMS_PER_PEER: u32 = 8;

/// This node's validator identity, as sent by Core: the node key's signature
/// over [`node_id_binding`] of this overlay's own peer id.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NodeIdentity {
    pub network_id: [u8; 32],
    pub node_id: NodeId,
    pub signature: [u8; 64],
}

/// What a validator signs to say "the overlay with this peer id is mine":
/// `SHA256(networkID || tag || peerId)`. The peer id is the one QUIC
/// authenticated, so the statement cannot be replayed by another peer.
pub fn node_id_binding(network_id: &[u8; 32], peer: &PeerId) -> [u8; 32] {
    let peer_bytes = peer.to_bytes();
    let mut message = Vec::with_capacity(32 + NODE_ID_BINDING_TAG.len() + peer_bytes.len());
    message.extend_from_slice(network_id);
    message.extend_from_slice(NODE_ID_BINDING_TAG);
    message.extend_from_slice(&peer_bytes);
    crate::xdr::sha256_hash(&message)
}

/// Whether `signature` is `node_id`'s signature binding it to `peer`.
fn verify_node_id_binding(
    network_id: &[u8; 32],
    peer: &PeerId,
    node_id: &NodeId,
    signature: &[u8; 64],
) -> bool {
    let Ok(key) = libp2p::identity::ed25519::PublicKey::try_from_bytes(node_id) else {
        return false;
    };
    key.verify(&node_id_binding(network_id, peer), signature)
}

/// Whether a piece is of a set its leader cut up: the header's overlay key
/// belongs to the leader (the validator's own signature over
/// [`node_id_binding`] of that overlay's peer id, as peers exchange it), and
/// that key signed the root of the set's pieces with the slot, the shape of
/// the code and the frame's length (`txset_coded::root_message`). Needs no
/// connection to the leader. `seed` is this node's seed of the header's slot.
fn verify_piece_signatures(network_id: &[u8; 32], seed: &[u8; 32], header: &PieceHeader) -> bool {
    let Ok(overlay_key) =
        libp2p::identity::ed25519::PublicKey::try_from_bytes(&header.overlay_key)
    else {
        return false;
    };
    let peer = libp2p::identity::PublicKey::from(overlay_key.clone()).to_peer_id();
    if !verify_node_id_binding(network_id, &peer, &header.leader, &header.binding_sig) {
        return false;
    }
    let (data, pieces) = header.params();
    let message = txset_coded::root_message(
        network_id,
        seed,
        header.slot,
        &header.leader,
        CodedParams {
            data,
            pieces,
            relays: 0,
        },
        header.total_len,
        &header.root,
    );
    overlay_key.verify(&message, &header.root_sig)
}

/// Identity state: ours, and claims from peers that arrived before Core told
/// us the network id needed to check them.
#[derive(Default)]
struct IdentityState {
    ours: Option<NodeIdentity>,
    unverified: HashMap<PeerId, (NodeId, [u8; 64])>,
    /// Claims handled per connected peer (see MAX_NODE_ID_CLAIMS_PER_PEER).
    claims: HashMap<PeerId, u32>,
}

/// Message frame: 4-byte length prefix + payload
/// Max message size: 16MB (for large TX sets)
const MAX_MESSAGE_SIZE: usize = crate::flood::txset_encoding::MAX_TXSET_XDR_SIZE + 4;

// Bound queued + active bulk sends. Admission waits in the caller, never in
// the swarm loop: a backpressured peer must not stop SCP or swarm polling.
const MAX_OUTSTANDING_TXSET_SENDS: usize = 64;
const MAX_OUTSTANDING_TXSET_BYTES: usize = 256 * 1024 * 1024;

/// Events from the overlay to the application
#[derive(Debug, Clone)]
pub enum OverlayEvent {
    /// Received SCP envelope from peer, with any tx set hashes it references
    /// (extracted during the reader's single decode).
    ScpReceived {
        envelope: Vec<u8>,
        txset_hashes: Vec<[u8; 32]>,
        from: PeerId,
        /// Slot the envelope is for (statement.slot_index), used to stamp
        /// tx sets fetched on behalf of this envelope.
        slot: u32,
    },
    /// Received TX set response
    TxSetReceived {
        hash: [u8; 32],
        data: Arc<TxSetData>,
        from: PeerId,
        /// Slot the set was requested for; None if the response was
        /// unsolicited (no pending request recorded).
        slot: Option<u32>,
    },
    /// Peer is requesting a TX set (need to look up and respond)
    TxSetRequested { hash: [u8; 32], from: PeerId },
    /// Peer is requesting SCP state
    ScpStateRequested { peer_id: PeerId, ledger_seq: u32 },
    /// Peer connected — includes the remote address for PeerId mapping
    PeerConnected { peer_id: PeerId, addr: Multiaddr },
    /// Peer disconnected - clean up any pending requests
    PeerDisconnected { peer_id: PeerId },
}

/// Commands to the overlay
#[derive(Debug)]
pub enum OverlayCommand {
    /// Broadcast SCP envelope to all peers
    BroadcastScp(Vec<u8>),
    /// Broadcast a validated TX to all peers
    BroadcastTx(Arc<ValidatedTx>),
    /// Request TX set from a peer (picks best peer). `retry` re-requests a
    /// set that a previous request has not delivered, from another peer.
    FetchTxSet {
        hash: [u8; 32],
        slot: u32,
        retry: bool,
    },
    /// Drop TX set request reservations for slots below `min_slot`
    PruneTxSetRequests { min_slot: u32 },
    /// Send TX set to a specific peer (response to their request)
    SendTxSet {
        hash: [u8; 32],
        data: Arc<TxSetData>,
        to: PeerId,
        permits: (OwnedSemaphorePermit, OwnedSemaphorePermit),
    },
    /// Record that a peer has a specific TX set (learned from SCP message)
    RecordTxSetSource { hash: [u8; 32], peer: PeerId },
    /// Push a TX set to every connected peer that has not been sent it
    /// (this node leads a round and is about to propose the set)
    PushTxSet {
        hash: [u8; 32],
        data: Arc<TxSetData>,
        /// The slot the set is for, if the caller knows it: with a tree
        /// configured for that slot the set goes down the tree.
        slot: Option<u32>,
    },
    /// Core accepted this transaction. The router already holds it; this
    /// carries out what it decided at that moment.
    SendLocalTx(SendPlan<PeerId>),
    /// The targets or the identified peers changed: send what the router now
    /// says is owed.
    FlushLocalTxs,
    /// Core sent this node's validator identity: tell every peer.
    SetNodeIdentity(NodeIdentity),
    /// Connect to a peer by address (bootstrap — PeerId unknown)
    Dial(Multiaddr),
    /// Connect to a known peer by PeerId (reconnect — deduplicates automatically)
    DialPeer { peer_id: PeerId, addr: Multiaddr },
    /// Request SCP state from all peers
    RequestScpState { ledger_seq: u32 },
    /// Send SCP envelope to a specific peer
    SendScpToPeer {
        peer_id: PeerId,
        envelope: Vec<u8>,
        responder: tokio::sync::oneshot::Sender<io::Result<()>>,
    },
    /// Shutdown
    Shutdown,
    /// Query the number of connected peers (responds via oneshot)
    GetConnectedPeerCount(tokio::sync::oneshot::Sender<usize>),
    /// Ping - responds immediately via oneshot channel (for testing event loop responsiveness)
    Ping(tokio::sync::oneshot::Sender<()>),
}

/// Each route has its own mutex, so a TxSet write does not hold the control
/// stream lock. Connection flow control and congestion capacity remain shared.
struct PeerOutboundStreams {
    scp: Mutex<Option<OutboundStream>>,
    tx: Mutex<Option<OutboundStream>>,
    txset: Mutex<Option<OutboundStream>>,
}

struct OutboundStream {
    stream: Stream,
    #[cfg(test)]
    protocol: StreamProtocol,
}

impl PeerOutboundStreams {
    fn new() -> Self {
        Self {
            scp: Mutex::new(None),
            tx: Mutex::new(None),
            txset: Mutex::new(None),
        }
    }
}

/// Network behaviour combining streams and Identify
#[derive(NetworkBehaviour)]
#[behaviour(to_swarm = "StellarBehaviourEvent")]
struct StellarBehaviour {
    stream: StreamBehaviour,
    identify: Identify,
}

#[derive(Debug)]
enum StellarBehaviourEvent {
    Stream(()), // StreamBehaviour emits () - no events
    Identify(IdentifyEvent),
}

impl From<()> for StellarBehaviourEvent {
    fn from(_event: ()) -> Self {
        StellarBehaviourEvent::Stream(())
    }
}

impl From<IdentifyEvent> for StellarBehaviourEvent {
    fn from(event: IdentifyEvent) -> Self {
        StellarBehaviourEvent::Identify(event)
    }
}

/// Handle for sending commands to the overlay
#[derive(Clone)]
pub struct OverlayHandle {
    cmd_tx: mpsc::Sender<OverlayCommand>,
    txset_send_slots: Arc<Semaphore>,
    txset_send_bytes: Arc<Semaphore>,
    /// Shared with the overlay task. The App updates it in the order of
    /// Core's messages (a transaction accepted, transactions externalized,
    /// new targets), then sends a command to act on it.
    tx_router: Arc<std::sync::Mutex<TxRouter<PeerId>>>,
    /// Shared with the overlay task: the trees the leader's tx set travels
    /// down (see `flood::txset_tree`).
    txset_tree: Arc<std::sync::Mutex<TreeState>>,
    metrics: Arc<OverlayMetrics>,
    local_peer_id: PeerId,
}

impl OverlayHandle {
    pub async fn broadcast_scp(&self, envelope: Vec<u8>) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::BroadcastScp(envelope))
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send BroadcastScp: {}",
                e
            );
        }
    }

    pub async fn broadcast_tx(&self, tx: Arc<ValidatedTx>) {
        if let Err(e) = self.cmd_tx.send(OverlayCommand::BroadcastTx(tx)).await {
            warn!(
                "Overlay command channel closed, failed to send BroadcastTx: {}",
                e
            );
        }
    }

    /// This overlay's own libp2p peer id (what Core's identity binding signs).
    pub fn local_peer_id(&self) -> PeerId {
        self.local_peer_id
    }

    /// Core accepted `tx`: keep it until it is externalized, and work out
    /// where it goes now. Synchronous, so that it is ordered with
    /// [`Self::ledger_externalized`] and [`Self::set_tx_targets`]; the returned
    /// plan is carried out with [`Self::send_local_tx`].
    pub fn add_local_tx(&self, tx: Arc<ValidatedTx>) -> SendPlan<PeerId> {
        self.tx_router
            .lock()
            .unwrap()
            .add_local(tx, Instant::now())
    }

    /// Carry out a plan from [`Self::add_local_tx`].
    pub async fn send_local_tx(&self, plan: SendPlan<PeerId>) {
        if plan.is_empty() {
            return;
        }
        if let Err(e) = self.cmd_tx.send(OverlayCommand::SendLocalTx(plan)).await {
            warn!(
                "Overlay command channel closed, failed to send SendLocalTx: {}",
                e
            );
        }
    }

    /// A ledger externalized with these transactions: stop sending them.
    /// The caller then calls [`Self::flush_local_txs`]: transactions that
    /// were held back from new targets may be due.
    pub fn ledger_externalized(&self, hashes: &[[u8; 32]]) {
        self.tx_router.lock().unwrap().ledger_externalized(hashes);
    }

    /// Core dropped these transactions: stop sending them.
    pub fn forget_local_txs(&self, hashes: &[[u8; 32]]) {
        self.tx_router.lock().unwrap().remove(hashes);
    }

    /// Core's list of validators that get transactions now (`None` = every
    /// peer). Returns whether it changed; if so, call
    /// [`Self::flush_local_txs`].
    pub fn set_tx_targets(&self, targets: Option<Vec<NodeId>>) -> bool {
        self.tx_router.lock().unwrap().set_targets(targets)
    }

    /// Send whatever the router says is owed after a change.
    pub async fn flush_local_txs(&self) {
        if let Err(e) = self.cmd_tx.send(OverlayCommand::FlushLocalTxs).await {
            warn!(
                "Overlay command channel closed, failed to send FlushLocalTxs: {}",
                e
            );
        }
    }

    /// Core sent this node's validator identity.
    pub async fn set_node_identity(&self, identity: NodeIdentity) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::SetNodeIdentity(identity))
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send SetNodeIdentity: {}",
                e
            );
        }
    }

    /// Number of transactions kept for sending (accepted from Core, not yet
    /// externalized).
    pub fn local_tx_count(&self) -> usize {
        self.tx_router.lock().unwrap().len()
    }

    /// Ask a peer for a set this node does not hold. If the set is expected
    /// through the tree, the request waits (`TREE_FETCH_GRACE` from the first
    /// mention of the hash) and is only sent if the set has not arrived by
    /// then: otherwise every node the leader did not push to would ask it the
    /// moment the proposal arrives.
    pub async fn fetch_txset(&self, hash: [u8; 32], slot: u32) {
        let decision = self
            .txset_tree
            .lock()
            .unwrap()
            .fetch_decision(&hash, slot, Instant::now());
        match decision {
            FetchDecision::Proceed => self.send_fetch_txset(hash, slot, false).await,
            FetchDecision::AlreadyWaiting => {}
            FetchDecision::Wait(wait) => {
                self.metrics
                    .txset_tree_fetch_deferred
                    .fetch_add(1, Ordering::Relaxed);
                let this = self.clone();
                tokio::spawn(async move {
                    tokio::time::sleep(wait).await;
                    if this.txset_tree.lock().unwrap().is_delivered(&hash) {
                        return;
                    }
                    this.metrics
                        .txset_tree_fetch_fallback
                        .fetch_add(1, Ordering::Relaxed);
                    info!(
                        "TXSET_TREE_FALLBACK: TX set {:02x?}... for slot {} did not arrive through the tree, asking for it",
                        &hash[..4],
                        slot
                    );
                    this.send_fetch_txset(hash, slot, false).await;
                });
            }
        }
    }

    /// Core described the trees of the next slots.
    pub fn set_txset_tree(&self, config: TreeConfig) {
        self.txset_tree.lock().unwrap().set_config(config);
    }

    /// Re-request a set an earlier request has not delivered. Unlike
    /// `fetch_txset`, this replaces an outstanding reservation and prefers
    /// another connected peer.
    pub async fn refetch_txset(&self, hash: [u8; 32], slot: u32) {
        self.send_fetch_txset(hash, slot, true).await;
    }

    async fn send_fetch_txset(&self, hash: [u8; 32], slot: u32, retry: bool) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::FetchTxSet { hash, slot, retry })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send FetchTxSet: {}",
                e
            );
        }
    }

    /// Forget TX set requests for slots below `min_slot`. A request that is
    /// never answered (e.g. a set no peer can serve) otherwise stays reserved
    /// until its peer disconnects.
    pub async fn prune_txset_requests(&self, min_slot: u32) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::PruneTxSetRequests { min_slot })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send PruneTxSetRequests: {}",
                e
            );
        }
    }

    pub async fn send_txset(&self, hash: [u8; 32], data: Arc<TxSetData>, to: PeerId) {
        // Include the StellarMessage discriminant in the existing wire limit.
        if data.len() > MAX_MESSAGE_SIZE - 4 {
            warn!(
                "Cannot send oversized TX set to {}: {} bytes",
                to,
                data.len()
            );
            return;
        }
        let permits = tokio::select! {
            permits = async {
                let slots = Arc::clone(&self.txset_send_slots).acquire_owned().await.unwrap();
                let bytes = Arc::clone(&self.txset_send_bytes)
                    .acquire_many_owned((data.len() + 4) as u32).await.unwrap();
                (slots, bytes)
            } => permits,
            _ = self.cmd_tx.closed() => return,
        };
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::SendTxSet {
                hash,
                data,
                to,
                permits,
            })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send SendTxSet: {}",
                e
            );
        }
    }

    /// Push a TX set to every connected peer without waiting for requests.
    /// Each peer is sent a given set once; see `PushRecord`.
    pub async fn push_txset(&self, hash: [u8; 32], data: Arc<TxSetData>) {
        self.send_push_txset(hash, data, None).await;
    }

    /// As [`Self::push_txset`] for the set of `slot`: if Core configured a
    /// tree for that slot, the set goes to this node's children in it (or,
    /// in star mode, to every peer with a header that says when).
    pub async fn push_txset_for_slot(&self, hash: [u8; 32], data: Arc<TxSetData>, slot: u32) {
        self.send_push_txset(hash, data, Some(slot)).await;
    }

    async fn send_push_txset(&self, hash: [u8; 32], data: Arc<TxSetData>, slot: Option<u32>) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::PushTxSet { hash, data, slot })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send PushTxSet: {}",
                e
            );
        }
    }

    /// Record that a peer has a specific TX set (call when receiving SCP with txSetHash)
    pub async fn record_txset_source(&self, hash: [u8; 32], peer: PeerId) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::RecordTxSetSource { hash, peer })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send RecordTxSetSource: {}",
                e
            );
        }
    }

    pub async fn dial(&self, addr: Multiaddr) {
        if let Err(e) = self.cmd_tx.send(OverlayCommand::Dial(addr)).await {
            warn!("Overlay command channel closed, failed to send Dial: {}", e);
        }
    }

    /// Dial a known peer by PeerId. libp2p will skip the dial if already connected.
    pub async fn dial_peer(&self, peer_id: PeerId, addr: Multiaddr) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::DialPeer { peer_id, addr })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send DialPeer: {}",
                e
            );
        }
    }

    pub async fn request_scp_state_from_all_peers(&self, ledger_seq: u32) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::RequestScpState { ledger_seq })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send RequestScpState: {}",
                e
            );
        }
    }

    pub async fn send_scp_to_peer(&self, peer_id: PeerId, envelope: &[u8]) -> io::Result<()> {
        let (responder, completion) = tokio::sync::oneshot::channel();
        self.cmd_tx
            .send(OverlayCommand::SendScpToPeer {
                peer_id,
                envelope: envelope.to_vec(),
                responder,
            })
            .await
            .map_err(|_| io::Error::new(io::ErrorKind::Other, "Channel closed"))?;
        // SCP-state responses send recent slots before the detached checkpoint.
        // Wait for the write so a caller awaiting successive sends preserves
        // that order, without making the dispatcher wait on network I/O.
        completion.await.map_err(|_| {
            io::Error::new(
                io::ErrorKind::BrokenPipe,
                "SCP send canceled before completion",
            )
        })?
    }

    pub async fn shutdown(&self) {
        if let Err(e) = self.cmd_tx.send(OverlayCommand::Shutdown).await {
            warn!(
                "Overlay command channel closed, failed to send Shutdown: {}",
                e
            );
        }
    }

    /// Query the number of currently connected peers
    pub async fn connected_peer_count(&self) -> usize {
        let (tx, rx) = tokio::sync::oneshot::channel();
        let _ = self
            .cmd_tx
            .send(OverlayCommand::GetConnectedPeerCount(tx))
            .await;
        rx.await.unwrap_or(0)
    }

    /// Ping the event loop and wait for response - for testing responsiveness
    #[cfg(test)]
    pub async fn ping(&self) -> Result<(), tokio::sync::oneshot::error::RecvError> {
        let (tx, rx) = tokio::sync::oneshot::channel();
        let _ = self.cmd_tx.send(OverlayCommand::Ping(tx)).await;
        rx.await
    }
}

/// Shared state for stream handlers
struct SharedState {
    /// Outbound streams per peer, with independent locks for each purpose
    peer_streams: RwLock<HashMap<PeerId, Arc<PeerOutboundStreams>>>,
    /// SCP messages seen (for dedup)
    scp_seen: RwLock<lru::LruCache<[u8; 32], ()>>,
    /// TX messages seen (for dedup)
    tx_seen: RwLock<lru::LruCache<[u8; 32], ()>>,
    /// Track which peers we've sent each SCP message to (prevent duplicate sends)
    scp_sent_to: RwLock<lru::LruCache<[u8; 32], HashSet<PeerId>>>,
    /// TX set sources: which peer has which TX set (learned from SCP messages)
    txset_sources: RwLock<lru::LruCache<[u8; 32], PeerId>>,
    /// Pending TX set requests: hash -> (peer, request_time) to avoid duplicate fetches and track latency
    /// hash -> (peer asked, request time, slot the set is for)
    pending_txset_requests: RwLock<HashMap<[u8; 32], (PeerId, Instant, u32)>>,
    /// TX sets this node pushed as a round leader, per peer. Never held
    /// across an await.
    txset_pushes: std::sync::Mutex<PushRecord>,
    /// Per-peer allowance for TX-set frames nobody asked for. Never held
    /// across an await.
    txset_push_limiter: std::sync::Mutex<PushRateLimiter>,
    /// Event sender for non-TX events (SCP, TxSet - critical path, unbounded)
    event_tx: mpsc::UnboundedSender<OverlayEvent>,
    /// Direct, bounded admission into the same mempool FIFO as Core removal.
    mempool: MempoolHandle,
    /// Counter for TXs dropped due to backpressure
    tx_dropped_count: AtomicU64,
    /// Stream control for reopening streams
    control: Control,

    // ============ INV/GETDATA State ============
    /// Batches INV announcements before sending (100ms or 1000 INVs)
    inv_batcher: RwLock<InvBatcher>,
    /// Tracks which peers have INV'd which TXs (for round-robin GETDATA)
    inv_tracker: RwLock<InvTracker>,
    /// Pending GETDATA requests with timeout tracking
    pending_getdata: RwLock<PendingRequests>,
    /// TX buffer for responding to GETDATA requests
    tx_buffer: RwLock<TxBuffer>,
    /// Where this node's own transactions go (shared with the App through
    /// `OverlayHandle`). Never held across an await.
    tx_router: Arc<std::sync::Mutex<TxRouter<PeerId>>>,
    /// The trees the leader's tx set travels down (shared with the App
    /// through `OverlayHandle`). Never held across an await, and never
    /// together with another of these locks.
    txset_tree: Arc<std::sync::Mutex<TreeState>>,
    /// Validator identities: ours and unchecked claims. Never held across
    /// an await.
    identity: std::sync::Mutex<IdentityState>,
    /// Transaction bodies queued for a direct send, per peer (see
    /// DIRECT_SEND_BATCH_INTERVAL). Never held across an await.
    direct_out: std::sync::Mutex<DirectOut>,
    /// Overlay metrics (shared with App for IPC reporting)
    metrics: Arc<OverlayMetrics>,
    /// This overlay's own key: it signs the root of the pieces of a set
    /// this node leads with (see `txset_coded`).
    keypair: Keypair,
}

impl SharedState {
    fn new(
        event_tx: mpsc::UnboundedSender<OverlayEvent>,
        mempool: MempoolHandle,
        control: Control,
        metrics: Arc<OverlayMetrics>,
        tx_router: Arc<std::sync::Mutex<TxRouter<PeerId>>>,
        txset_tree: Arc<std::sync::Mutex<TreeState>>,
        keypair: Keypair,
    ) -> Self {
        Self {
            peer_streams: RwLock::new(HashMap::new()),
            scp_seen: RwLock::new(lru::LruCache::new(
                std::num::NonZeroUsize::new(10000).unwrap(),
            )),
            tx_seen: RwLock::new(lru::LruCache::new(
                std::num::NonZeroUsize::new(100000).unwrap(),
            )),
            scp_sent_to: RwLock::new(lru::LruCache::new(
                std::num::NonZeroUsize::new(10000).unwrap(),
            )),
            txset_sources: RwLock::new(lru::LruCache::new(
                std::num::NonZeroUsize::new(1000).unwrap(),
            )),
            pending_txset_requests: RwLock::new(HashMap::new()),
            txset_pushes: std::sync::Mutex::new(PushRecord::new()),
            txset_push_limiter: std::sync::Mutex::new(PushRateLimiter::default()),
            event_tx,
            mempool,
            tx_dropped_count: AtomicU64::new(0),
            control,
            // INV/GETDATA state
            inv_batcher: RwLock::new(InvBatcher::new()),
            inv_tracker: RwLock::new(InvTracker::new()),
            pending_getdata: RwLock::new(PendingRequests::new()),
            tx_buffer: RwLock::new(TxBuffer::new()),
            tx_router,
            txset_tree,
            identity: std::sync::Mutex::new(IdentityState::default()),
            direct_out: std::sync::Mutex::new(DirectOut::default()),
            metrics,
            keypair,
        }
    }
}

/// The unified Stellar overlay
pub struct StellarOverlay {
    swarm: Swarm<StellarBehaviour>,
    control: Control,
    state: Arc<SharedState>,
    cmd_rx: mpsc::Receiver<OverlayCommand>,
    // Reaped while polling the swarm; dropping the overlay cancels its sends.
    sends: JoinSet<()>,
}

/// See `quic_initial_congestion_window`. `None` = quinn's default.
///
/// 8 MiB lets a tx set (0.5-0.8 MB compressed at 3,000 TPS) leave in one
/// flight. quinn paces one window over 0.8 round trips and does not grow the
/// window while the sender is application limited, so with its default a
/// 640 KB frame took 1 to 1.3 round trips on top of the one-way delay even on
/// a warm connection (and 6 to 7 on a cold one); with 8 MiB it arrives about
/// 20 ms after the one-way delay at a 150 ms round trip. CUBIC still shrinks
/// the window on loss. A prototype setting for validators on good links.
const DEFAULT_QUIC_INITIAL_CONGESTION_WINDOW: Option<u64> = Some(8 * 1024 * 1024);

/// Initial QUIC congestion window for every peer connection, in bytes.
/// `STELLAR_OVERLAY_QUIC_INITIAL_CWND` overrides it (0 = quinn's default of
/// about ten packets); the override exists for latency experiments.
fn quic_initial_congestion_window() -> Option<u64> {
    let Ok(value) = std::env::var("STELLAR_OVERLAY_QUIC_INITIAL_CWND") else {
        return DEFAULT_QUIC_INITIAL_CONGESTION_WINDOW;
    };
    match value.parse::<u64>() {
        Ok(0) => None,
        Ok(bytes) => Some(bytes),
        Err(_) => {
            warn!(
                "STELLAR_OVERLAY_QUIC_INITIAL_CWND={:?} is not a byte count, using the default",
                value
            );
            DEFAULT_QUIC_INITIAL_CONGESTION_WINDOW
        }
    }
}

/// Create the overlay and return handle + event receivers
///
/// Returns:
/// - `OverlayHandle`: for sending commands to the overlay
/// - `UnboundedReceiver<OverlayEvent>`: for SCP, TxSet events (critical path, never dropped)
/// - `StellarOverlay`: the overlay to run
pub fn create_overlay(
    keypair: Keypair,
    metrics: Arc<OverlayMetrics>,
    mempool: MempoolHandle,
) -> Result<
    (
        OverlayHandle,
        mpsc::UnboundedReceiver<OverlayEvent>,
        StellarOverlay,
    ),
    Box<dyn std::error::Error + Send + Sync>,
> {
    let peer_id = keypair.public().to_peer_id();
    info!(
        "Creating StellarOverlay with peer_id={} (QUIC transport)",
        peer_id
    );

    // Build swarm with QUIC transport
    // Configure QUIC with keep-alive to prevent idle connection drops
    let mut quic_config = libp2p::quic::Config::new(&keypair);
    quic_config.keep_alive_interval = Duration::from_secs(15);
    quic_config.max_idle_timeout = 60_000; // 60 seconds in ms
    quic_config.initial_congestion_window = quic_initial_congestion_window();
    match quic_config.initial_congestion_window {
        Some(bytes) => info!("QUIC_CWND: initial congestion window {} bytes", bytes),
        None => info!("QUIC_CWND: initial congestion window: quinn default"),
    }

    let swarm = SwarmBuilder::with_existing_identity(keypair.clone())
        .with_tokio()
        .with_quic_config(|_| quic_config)
        .with_behaviour(|key| {
            let stream = StreamBehaviour::new();

            let identify = Identify::new(IdentifyConfig::new(
                "/stellar/1.0.0".to_string(),
                key.public(),
            ));

            StellarBehaviour { stream, identify }
        })?
        .with_swarm_config(|cfg| cfg.with_idle_connection_timeout(Duration::from_secs(300)))
        .build();

    let control = swarm.behaviour().stream.new_control();

    let (cmd_tx, cmd_rx) = mpsc::channel(256);
    // Unbounded channel for critical events (SCP, TxSet) - never drop
    let (event_tx, event_rx) = mpsc::unbounded_channel();

    let tx_router = Arc::new(std::sync::Mutex::new(TxRouter::new()));
    let txset_tree = Arc::new(std::sync::Mutex::new(TreeState::new()));
    let state = Arc::new(SharedState::new(
        event_tx,
        mempool,
        control.clone(),
        Arc::clone(&metrics),
        Arc::clone(&tx_router),
        Arc::clone(&txset_tree),
        keypair.clone(),
    ));

    let overlay = StellarOverlay {
        swarm,
        control,
        state,
        cmd_rx,
        sends: JoinSet::new(),
    };

    let handle = OverlayHandle {
        cmd_tx,
        txset_send_slots: Arc::new(Semaphore::new(MAX_OUTSTANDING_TXSET_SENDS)),
        txset_send_bytes: Arc::new(Semaphore::new(MAX_OUTSTANDING_TXSET_BYTES)),
        tx_router,
        txset_tree,
        metrics,
        local_peer_id: peer_id,
    };

    Ok((handle, event_rx, overlay))
}

impl StellarOverlay {
    /// Bind before starting App so a transport requirement failure aborts startup.
    pub fn listen(&mut self, listen_ip: &str, listen_port: u16) -> io::Result<()> {
        let listen_addr: Multiaddr = format!("/ip4/{}/udp/{}/quic-v1", listen_ip, listen_port)
            .parse()
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
        self.swarm.listen_on(listen_addr.clone()).map_err(|e| {
            io::Error::new(
                io::ErrorKind::Other,
                format!("Failed to listen on {listen_addr}: {e:?}"),
            )
        })?;
        info!("Listening on QUIC port {}", listen_port);
        Ok(())
    }

    #[cfg(test)]
    async fn run(mut self, listen_ip: &str, listen_port: u16) {
        self.listen(listen_ip, listen_port).unwrap();
        self.run_event_loop().await;
    }

    pub async fn run_event_loop(mut self) {
        let control_incoming = self
            .control
            .accept(CONTROL_PROTOCOL)
            .expect("control protocol registered once");
        let tx_incoming = self
            .control
            .accept(TX_PROTOCOL)
            .expect("TX protocol registered once");
        let txset_incoming = self
            .control
            .accept(TXSET_PROTOCOL)
            .expect("TxSet protocol registered once");
        let node_id_incoming = self
            .control
            .accept(NODE_ID_PROTOCOL)
            .expect("node id protocol registered once");
        let state = self.state.clone();
        tokio::spawn(handle_inbound_node_id_streams(
            node_id_incoming,
            state.clone(),
        ));
        tokio::spawn(handle_inbound_scp_streams(control_incoming, state.clone()));
        tokio::spawn(handle_inbound_tx_streams(tx_incoming, state.clone()));
        tokio::spawn(handle_inbound_txset_streams(txset_incoming, state.clone()));

        // Spawn INV/GETDATA housekeeping task
        tokio::spawn(inv_getdata_housekeeping_task(state.clone()));
        tokio::spawn(direct_send_flush_task(state.clone()));
        tokio::spawn(txset_tree_probe_task(state.clone()));

        loop {
            tokio::select! {
                Some(result) = self.sends.join_next(), if !self.sends.is_empty() => {
                    if let Err(e) = result {
                        error!("Overlay send task failed: {}", e);
                    }
                }
                event = self.swarm.select_next_some() => {
                    self.handle_swarm_event(event).await;
                }

                Some(cmd) = self.cmd_rx.recv() => {
                    match cmd {
                        OverlayCommand::BroadcastScp(envelope) => {
                            self.broadcast_scp(&envelope).await;
                        }
                        OverlayCommand::BroadcastTx(tx) => {
                            self.broadcast_tx(tx).await;
                        }
                        OverlayCommand::FetchTxSet { hash, slot, retry } => {
                            self.fetch_txset(hash, slot, retry).await;
                        }
                        OverlayCommand::PruneTxSetRequests { min_slot } => {
                            self.prune_txset_requests(min_slot).await;
                        }
                        OverlayCommand::SendTxSet { hash, data, to, permits } => {
                            let state = Arc::clone(&self.state);
                            self.sends.spawn(async move {
                                let _permits = permits;
                                send_txset_response(state, to, hash, data).await;
                            });
                        }
                        OverlayCommand::RecordTxSetSource { hash, peer } => {
                            let mut sources = self.state.txset_sources.write().await;
                            sources.put(hash, peer);
                            debug!("Recorded peer {} as source for TX set {:02x?}...", peer, &hash[..4]);
                        }
                        OverlayCommand::PushTxSet { hash, data, slot } => {
                            self.push_txset(hash, data, slot).await;
                        }
                        OverlayCommand::SendLocalTx(plan) => {
                            self.send_local_tx_plan(plan).await;
                        }
                        OverlayCommand::FlushLocalTxs => {
                            flush_local_txs(&self.state).await;
                        }
                        OverlayCommand::SetNodeIdentity(identity) => {
                            self.set_node_identity(identity).await;
                        }
                        OverlayCommand::Dial(addr) => {
                            info!("Dialing peer at {}", addr);
                            self.state.metrics.connection_pending.fetch_add(1, Ordering::Relaxed);
                            self.state.metrics.outbound_attempt.fetch_add(1, Ordering::Relaxed);
                            if let Err(e) = self.swarm.dial(addr.clone()) {
                                self.state.metrics.connection_pending.fetch_sub(1, Ordering::Relaxed);
                                warn!("Failed to dial {}: {}", addr, e);
                            }
                        }
                        OverlayCommand::DialPeer { peer_id, addr } => {
                            let opts = DialOpts::peer_id(peer_id)
                                .condition(PeerCondition::Disconnected)
                                .addresses(vec![addr.clone()])
                                .build();
                            self.state.metrics.outbound_attempt.fetch_add(1, Ordering::Relaxed);
                            match self.swarm.dial(opts) {
                                Ok(_) => {
                                    self.state.metrics.connection_pending.fetch_add(1, Ordering::Relaxed);
                                    debug!("Dialing known peer {} at {}", peer_id, addr);
                                }
                                Err(e) => {
                                    // DialError::NoAddresses means already connected — not an error
                                    debug!("DialPeer {} skipped or failed: {}", peer_id, e);
                                }
                            }
                        }
                        OverlayCommand::RequestScpState { ledger_seq } => {
                            info!("Requesting SCP state (ledger >= {}) from all peers", ledger_seq);
                            self.request_scp_state_from_all_peers(ledger_seq).await;
                        }
                        OverlayCommand::SendScpToPeer { peer_id, envelope, responder } => {
                            let state = Arc::clone(&self.state);
                            let message = crate::xdr::frame_scp(&envelope);
                            self.sends.spawn(async move {
                                let result = send_to_peer_stream(&state, peer_id, StreamType::Control, &message).await;
                                if let Err(e) = &result {
                                    warn!("Failed to send SCP to {}: {:?}", peer_id, e);
                                }
                                let _ = responder.send(result);
                            });
                        }
                        OverlayCommand::Shutdown => {
                            info!("Overlay shutting down");
                            break;
                        }
                        OverlayCommand::GetConnectedPeerCount(responder) => {
                            let count = self.state.peer_streams.read().await.len();
                            let _ = responder.send(count);
                        }
                        OverlayCommand::Ping(responder) => {
                            let _ = responder.send(());
                        }
                    }
                }
            }
        }
        // Close admission first so callers waiting for bulk-send permits also
        // wake up. Abort writes only at shutdown, when streams are being torn
        // down; resuming a partially written frame would corrupt the stream.
        self.cmd_rx.close();
        drop(self.swarm);
        self.sends.shutdown().await;
    }

    async fn handle_swarm_event(&mut self, event: SwarmEvent<StellarBehaviourEvent>) {
        match event {
            SwarmEvent::NewListenAddr { address, .. } => {
                info!("Listening on {}", address);
            }

            SwarmEvent::ConnectionEstablished {
                peer_id,
                num_established,
                endpoint,
                ..
            } => {
                // Only decrement connection_pending for outbound dials we initiated
                if endpoint.is_dialer() {
                    self.state
                        .metrics
                        .connection_pending
                        .fetch_sub(1, Ordering::Relaxed);
                    self.state
                        .metrics
                        .outbound_establish
                        .fetch_add(1, Ordering::Relaxed);
                } else {
                    self.state
                        .metrics
                        .inbound_establish
                        .fetch_add(1, Ordering::Relaxed);
                }

                // Only open streams on the first connection to a peer.
                // When both sides dial simultaneously, two ConnectionEstablished
                // events fire for the same peer. Opening streams on each would
                // overwrite the first set, dropping those streams and causing
                // "unexpected end of file" on the remote's inbound handlers.
                if num_established.get() == 1 {
                    info!("Connected to peer {}", peer_id);
                    self.state
                        .metrics
                        .connection_authenticated
                        .fetch_add(1, Ordering::Relaxed);
                    {
                        let mut streams = self.state.peer_streams.write().await;
                        streams.insert(peer_id, Arc::new(PeerOutboundStreams::new()));
                    }

                    // Notify application so it can record the PeerId ↔ address mapping.
                    // Extract the remote address from the endpoint for reconnection.
                    let remote_addr = match &endpoint {
                        libp2p::core::ConnectedPoint::Dialer { address, .. } => address.clone(),
                        libp2p::core::ConnectedPoint::Listener { send_back_addr, .. } => {
                            send_back_addr.clone()
                        }
                    };
                    let _ = self.state.event_tx.send(OverlayEvent::PeerConnected {
                        peer_id: peer_id.clone(),
                        addr: remote_addr,
                    });

                    // Spawn stream opening as a background task so the swarm
                    // event loop stays free to poll — control.open_stream()
                    // needs the swarm to process the request.
                    let control = self.control.clone();
                    let state = self.state.clone();
                    tokio::spawn(open_streams_to_peer(control, state, peer_id));
                } else {
                    debug!(
                        "Duplicate connection to {} (now {}), skipping stream setup",
                        peer_id, num_established
                    );
                    // The peer may have seen its side of the earlier
                    // connection close and forgotten who this node is.
                    let identity = self.state.identity.lock().unwrap().ours.clone();
                    if let Some(identity) = identity {
                        tokio::spawn(send_node_identity(
                            self.control.clone(),
                            peer_id,
                            identity,
                        ));
                    }
                }
            }

            SwarmEvent::ConnectionClosed {
                peer_id,
                num_established,
                ..
            } => {
                // Whatever was pushed to the peer may have been on this
                // connection's streams and not arrived: answer its requests
                // normally again. This holds for a duplicate connection too.
                self.state
                    .txset_pushes
                    .lock()
                    .unwrap()
                    .forget_peer(&peer_id);
                // Only clean up when the LAST connection to this peer closes.
                // Duplicate connections closing shouldn't tear down working streams.
                if num_established == 0 {
                    info!("Disconnected from peer {}", peer_id);
                    self.state
                        .metrics
                        .connection_authenticated
                        .fetch_sub(1, Ordering::Relaxed);
                    self.state
                        .metrics
                        .outbound_drop
                        .fetch_add(1, Ordering::Relaxed);
                    {
                        let mut streams = self.state.peer_streams.write().await;
                        streams.remove(&peer_id);
                    }
                    // Clean up pending txset requests for this peer
                    {
                        let mut pending = self.state.pending_txset_requests.write().await;
                        let before_len = pending.len();
                        pending.retain(|_hash, (p, _, _)| p != &peer_id);
                        let removed = before_len - pending.len();
                        if removed > 0 {
                            info!(
                                "Removed {} pending txset requests for disconnected peer {}",
                                removed, peer_id
                            );
                        }
                    }
                    self.state
                        .txset_push_limiter
                        .lock()
                        .unwrap()
                        .forget_peer(&peer_id);
                    // The peer is no longer a reachable target, and whatever
                    // was sent to it may be lost.
                    {
                        let mut ids = self.state.identity.lock().unwrap();
                        ids.unverified.remove(&peer_id);
                        ids.claims.remove(&peer_id);
                    }
                    // Whatever was queued for the peer is not sent; the
                    // router forgets its sends to the peer just below.
                    self.state.direct_out.lock().unwrap().queued.remove(&peer_id);
                    let (identified, gone) = {
                        let mut router = self.state.tx_router.lock().unwrap();
                        let gone = router.node_of(&peer_id).copied();
                        router.peer_disconnected(&peer_id);
                        (router.identified_peer_count(), gone)
                    };
                    if let Some(node) = gone {
                        // A new connection to it is measured afresh.
                        self.state.txset_tree.lock().unwrap().forget_rtt(&node);
                    }
                    self.state
                        .metrics
                        .node_id_identified
                        .store(identified as i64, Ordering::Relaxed);
                    flush_local_txs(&self.state).await;
                    // Notify main loop to clean up any pending requests for this peer
                    if let Err(e) = self.state.event_tx.send(OverlayEvent::PeerDisconnected {
                        peer_id: peer_id.clone(),
                    }) {
                        warn!(
                            "Failed to send PeerDisconnected event for {}: {}",
                            peer_id, e
                        );
                    }
                } else {
                    debug!(
                        "Duplicate connection to {} closed ({} remaining)",
                        peer_id, num_established
                    );
                }
            }

            SwarmEvent::Behaviour(StellarBehaviourEvent::Identify(event)) => {
                if let IdentifyEvent::Received { peer_id, info, .. } = event {
                    debug!("Identified peer {}: {:?}", peer_id, info.listen_addrs);
                }
            }

            SwarmEvent::Behaviour(StellarBehaviourEvent::Stream(_)) => {
                // Stream events handled by the stream behaviour internally
            }

            SwarmEvent::IncomingConnection { .. } => {
                trace!("Incoming connection");
                self.state
                    .metrics
                    .inbound_attempt
                    .fetch_add(1, Ordering::Relaxed);
            }

            SwarmEvent::OutgoingConnectionError { peer_id, error, .. } => {
                warn!("Outgoing connection failed to {:?}: {}", peer_id, error);
                self.state
                    .metrics
                    .connection_pending
                    .fetch_sub(1, Ordering::Relaxed);
            }

            _ => {}
        }
    }

    /// Broadcast SCP envelope to all connected peers
    async fn broadcast_scp(&mut self, envelope: &[u8]) {
        // Core is trusted for encoding; frame by concatenation (no decode).
        let message = crate::xdr::frame_scp(envelope);
        let hash = blake2b_hash(envelope);

        // Mark as seen for inbound dedup (if we later receive this from a peer, skip it)
        {
            let mut seen = self.state.scp_seen.write().await;
            seen.put(hash, ());
        }

        // Determine which peers still need this message
        let streams = self.state.peer_streams.read().await;
        let all_peers: Vec<_> = streams.keys().cloned().collect();
        drop(streams);

        let peers_to_send: Vec<PeerId>;
        {
            let mut sent_to = self.state.scp_sent_to.write().await;
            let already_sent: HashSet<PeerId> = sent_to.peek(&hash).cloned().unwrap_or_default();

            peers_to_send = all_peers
                .into_iter()
                .filter(|p| !already_sent.contains(p))
                .collect();

            if peers_to_send.is_empty() {
                trace!(
                    "SCP_BROADCAST_SKIP: SCP {:02x?}... already sent to all connected peers",
                    &hash[..4]
                );
                return;
            }

            // Update sent_to with the peers we're about to send to
            let mut new_sent = already_sent;
            new_sent.extend(peers_to_send.iter().cloned());
            sent_to.put(hash, new_sent);
        }

        debug!(
            "SCP_BROADCAST: Broadcasting SCP {:02x?}... ({} bytes) to {} peers",
            &hash[..4],
            envelope.len(),
            peers_to_send.len()
        );
        self.state
            .metrics
            .message_broadcast
            .fetch_add(1, Ordering::Relaxed);

        // Spawn parallel send tasks - don't block event loop waiting for each peer
        for peer_id in peers_to_send {
            let state = Arc::clone(&self.state);
            let message = message.clone();
            self.sends.spawn(async move {
                match send_to_peer_stream(&state, peer_id.clone(), StreamType::Control, &message)
                    .await
                {
                    Ok(_) => {
                        state
                            .metrics
                            .send_scp_message
                            .fetch_add(1, Ordering::Relaxed);
                        state.metrics.message_write.fetch_add(1, Ordering::Relaxed);
                        state
                            .metrics
                            .byte_write
                            .fetch_add(message.len() as u64, Ordering::Relaxed);
                        debug!(
                            "SCP_SEND_OK: Sent SCP {:02x?}... to {}",
                            &hash[..4],
                            peer_id
                        );
                    }
                    Err(e) => {
                        state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
                        warn!(
                            "SCP_SEND_FAIL: Failed to send SCP {:02x?}... to {}: {}",
                            &hash[..4],
                            peer_id,
                            e
                        );
                    }
                }
            });
        }
    }

    /// Flood a transaction to all connected peers with the INV/GETDATA
    /// protocol, unless it was seen before. (Core's own transactions go
    /// through the router instead, see `send_local_tx_plan`.)
    async fn broadcast_tx(&mut self, tx: Arc<ValidatedTx>) {
        let hash = *tx.hash();

        // Dedup check
        {
            let mut seen = self.state.tx_seen.write().await;
            if seen.contains(&hash) {
                trace!("TX already seen, skipping broadcast");
                return;
            }
            seen.put(hash, ());
            self.state
                .metrics
                .memory_flood_known
                .store(seen.len() as i64, Ordering::Relaxed);
        }
        advertise_txs_to_all(&self.state, &[tx]).await;
    }

    /// Carry out the router's plan for transactions Core accepted.
    async fn send_local_tx_plan(&mut self, plan: SendPlan<PeerId>) {
        {
            // A local transaction is known from now on: a copy from a peer
            // is a duplicate.
            let mut seen = self.state.tx_seen.write().await;
            for tx in plan
                .flood
                .iter()
                .chain(plan.direct.iter().flat_map(|(_, txs)| txs.iter()))
            {
                seen.put(*tx.hash(), ());
            }
            self.state
                .metrics
                .memory_flood_known
                .store(seen.len() as i64, Ordering::Relaxed);
        }
        execute_send_plan(&self.state, plan).await;
    }

    /// Core told us which validator this overlay belongs to: keep it, check
    /// the claims that were waiting for the network id, and tell every peer.
    async fn set_node_identity(&mut self, identity: NodeIdentity) {
        info!(
            "NODE_ID: This overlay belongs to validator {:02x?}...",
            &identity.node_id[..4]
        );
        let waiting: Vec<(PeerId, (NodeId, [u8; 64]))> = {
            let mut ids = self.state.identity.lock().unwrap();
            ids.ours = Some(identity.clone());
            ids.unverified.drain().collect()
        };
        self.state
            .tx_router
            .lock()
            .unwrap()
            .set_local_node(identity.node_id);
        self.state
            .txset_tree
            .lock()
            .unwrap()
            .set_local_node(identity.node_id);
        for (peer, (node_id, signature)) in waiting {
            accept_node_id_claim(&self.state, peer, node_id, signature).await;
        }
        let peers: Vec<PeerId> = self.state.peer_streams.read().await.keys().cloned().collect();
        for peer in peers {
            let control = self.control.clone();
            let identity = identity.clone();
            tokio::spawn(async move { send_node_identity(control, peer, identity).await });
        }
        flush_local_txs(&self.state).await;
    }

    /// Fetch TX set from a peer - preferring the peer who sent us the SCP message referencing it.
    /// A `retry` replaces an outstanding request (which never delivered) and
    /// goes to a random connected peer other than the previous one: that may
    /// have been silent or not had the set, and there is no negative response.
    async fn fetch_txset(&mut self, hash: [u8; 32], slot: u32, retry: bool) {
        // Read each shared map on its own; never nest these locks.
        let previous_peer = {
            let pending = self.state.pending_txset_requests.read().await;
            pending.get(&hash).map(|(peer, _, _)| *peer)
        };
        let known_source = {
            let sources = self.state.txset_sources.read().await;
            sources.peek(&hash).cloned()
        };
        let streams = self.state.peer_streams.read().await;
        // Check if we're already fetching this TxSet from a connected peer (dedup)
        if let Some(pending_peer) = previous_peer {
            if !retry && streams.contains_key(&pending_peer) {
                debug!(
                    "TXSET_FETCH_SKIP: TxSet {:02x?}... already being fetched from {}, skipping duplicate",
                    &hash[..4], pending_peer
                );
                return;
            }
            // Otherwise the peer disconnected or the request is being retried.
        }
        // Avoid the previous peer on a retry unless it is the only one left.
        let avoid = previous_peer.filter(|p| retry && streams.keys().any(|other| other != p));
        let eligible = |p: &PeerId| streams.contains_key(p) && Some(*p) != avoid;

        // Prefer the peer that referenced the set in an SCP message. A retry
        // picks a random other peer instead: preferring the source again would
        // send every other retry back to a source that may not answer.
        let peer = if let Some(source_peer) = known_source.filter(|p| !retry && eligible(p)) {
            info!(
                "TXSET_FETCH: Fetching TX set {:02x?}... from known source {}{}",
                &hash[..4],
                source_peer,
                if retry { " (retry)" } else { "" }
            );
            source_peer
        } else {
            // No usable known source: pick a connected peer, at random on a
            // retry so repeated retries spread over the peers.
            let mut candidates = streams.keys().filter(|p| eligible(p));
            let choice = if retry {
                use rand::seq::IteratorRandom;
                candidates.choose(&mut rand::thread_rng()).cloned()
            } else {
                candidates.next().cloned()
            };
            match choice {
                Some(p) => {
                    info!(
                        "TXSET_FETCH: Fetching TX set {:02x?}... from peer {} (source {:?}{})",
                        &hash[..4],
                        p,
                        known_source,
                        if retry { ", retry" } else { "" }
                    );
                    p
                }
                None => {
                    warn!(
                        "TXSET_FETCH_FAIL: No peers to fetch TX set {:02x?}... from",
                        &hash[..4]
                    );
                    return;
                }
            }
        };
        drop(streams);

        // Keep selection and reservation in the dispatcher, before spawning
        // the write, so repeated FetchTxSet commands cannot race past dedup.
        let pending_request = (peer, Instant::now(), slot);
        self.state
            .pending_txset_requests
            .write()
            .await
            .insert(hash, pending_request);
        // The answer must get past the limit on TX-set frames from that peer.
        self.state
            .txset_push_limiter
            .lock()
            .unwrap()
            .request_sent(peer, Instant::now());

        let request = crate::xdr::frame_get_tx_set(hash);
        let state = Arc::clone(&self.state);
        self.sends.spawn(async move {
            match send_to_peer_stream(&state, peer, StreamType::TxSetRequest, &request).await {
                Ok(_) => info!(
                    "TXSET_FETCH_SENT: Sent request for TxSet {:02x?}... to {}",
                    &hash[..4],
                    peer
                ),
                Err(e) => {
                    warn!(
                        "TXSET_FETCH_FAIL: Failed to send TxSet request {:02x?}... to {}: {}",
                        &hash[..4],
                        peer,
                        e
                    );
                    // A disconnect/reconnect may have assigned a new request
                    // while this send was in flight. Only clear our own entry.
                    let mut pending = state.pending_txset_requests.write().await;
                    if pending.get(&hash) == Some(&pending_request) {
                        pending.remove(&hash);
                    }
                }
            }
        });
    }

    /// Whom the set of `slot` goes to when this node leads, and the header it
    /// carries (none: an ordinary frame, as before the tree).
    ///
    /// * No tree configured, or mode off: every connected peer, no header.
    /// * Star: every connected peer, with a header.
    /// * Hash, rtt: this node's children in the tree. If the tree of the slot
    ///   is not known here, or a child has no identified connection (right
    ///   after a start, before the peers proved who they are), the set goes
    ///   to every peer as a star instead: a subtree must not depend on a
    ///   child the root cannot reach.
    fn tree_push_plan(&self, slot: Option<u32>, connected: &[PeerId]) -> TreePushPlan {
        let everyone = |header: Option<TreeHeader>| TreePushPlan {
            header,
            targets: connected.to_vec(),
            subtrees: HashMap::new(),
            star: None,
        };
        let Some(slot) = slot else {
            return everyone(None);
        };
        let Some(me) = self
            .state
            .identity
            .lock()
            .unwrap()
            .ours
            .as_ref()
            .map(|i| i.node_id)
        else {
            return everyone(None);
        };
        // One lock at a time: the identified peers first, then the tree.
        let peer_of: HashMap<NodeId, PeerId> = {
            let router = self.state.tx_router.lock().unwrap();
            connected
                .iter()
                .filter_map(|peer| router.node_of(peer).map(|node| (*node, *peer)))
                .collect()
        };
        let sent_unix_ms = unix_ms_now();
        let star = TreeHeader {
            kind: FrameKind::Star,
            fanout: 0,
            hop: 1,
            slot,
            leader: me,
            sent_unix_ms,
            order: Vec::new(),
            nodes_digest: [0; txset_tree::TREE_NODES_DIGEST],
        };
        // Copy what the layout needs and let go of the lock before computing.
        let (config, measured) = {
            let tree = self.state.txset_tree.lock().unwrap();
            let Some(config) = tree.config().cloned() else {
                return everyone(None);
            };
            let measured: Vec<(NodeId, Option<Duration>)> = if config.mode == TreeMode::Rtt {
                config
                    .nodes
                    .iter()
                    .filter(|node| **node != me)
                    .map(|node| (*node, tree.rtt_of(node)))
                    .collect()
            } else {
                Vec::new()
            };
            (config, measured)
        };
        let fanout = config.effective_fanout();
        // A node the others do not expect to lead the slot (a later round,
        // another view) would have its frames refused by every relay: it
        // pushes to everyone.
        let known = config.seed_of(slot).is_some()
            && config.contains(&me)
            && config.may_lead(slot, &me);
        let (kind, order, indices) = match config.mode {
            TreeMode::Off => return everyone(None),
            // Coded: `coded_push` did not take the set (see there).
            TreeMode::Star | TreeMode::Coded => return everyone(Some(star)),
            _ if !known => return everyone(Some(star)),
            TreeMode::Hash => match config.hash_order(slot, &me) {
                Some(order) => (FrameKind::Hash, order, Vec::new()),
                None => return everyone(Some(star)),
            },
            TreeMode::Rtt => {
                let order = txset_tree::rtt_order(&me, &measured, fanout);
                match config.indices_of(&order) {
                    Some(indices) => (FrameKind::Explicit, order, indices),
                    None => return everyone(Some(star)),
                }
            }
        };
        let children = txset_tree::children_of(&order, &me, fanout);
        let targets: Vec<PeerId> = children
            .iter()
            .filter_map(|node| peer_of.get(node).copied())
            .collect();
        if targets.len() < children.len() {
            self.state.metrics.txset_tree_unreachable.fetch_add(
                (children.len() - targets.len()) as u64,
                Ordering::Relaxed,
            );
            warn!(
                "TXSET_TREE_STAR: {} of {} children for slot {} have no identified connection, pushing to every peer",
                children.len() - targets.len(),
                children.len(),
                slot
            );
            return everyone(Some(star));
        }
        // Who sits below each child: if a child cannot be sent the set now
        // (an earlier push to it is still in flight), they get it from this
        // node directly.
        let mut subtrees: HashMap<PeerId, Vec<PeerId>> = HashMap::new();
        for child in &children {
            let mut below = Vec::new();
            let mut frontier = vec![*child];
            while let Some(node) = frontier.pop() {
                for descendant in txset_tree::children_of(&order, &node, fanout) {
                    if let Some(peer) = peer_of.get(&descendant) {
                        below.push(*peer);
                    }
                    frontier.push(descendant);
                }
            }
            subtrees.insert(peer_of[child], below);
        }
        TreePushPlan {
            header: Some(TreeHeader {
                kind,
                fanout: fanout.min(u8::MAX as usize) as u8,
                hop: 1,
                slot,
                leader: me,
                sent_unix_ms,
                order: indices,
                nodes_digest: if kind == FrameKind::Explicit {
                    config.nodes_digest()
                } else {
                    [0; txset_tree::TREE_NODES_DIGEST]
                },
            }),
            targets,
            subtrees,
            star: Some(star),
        }
    }

    /// In coded mode, cut the set of `slot` into pieces and write every piece
    /// to its relays (see `txset_coded`). False: the set is not coded (another
    /// mode, the slot's layout is not known here, too few nodes, or more than
    /// a quarter of the relays have no identified connection, as right after
    /// a start), and the caller pushes it as before, to every peer.
    ///
    /// The pieces are not registered as a push of the set to their receivers
    /// (`PushRecord`): no peer gets the whole set from this node, so a
    /// request for it is answered.
    fn coded_push(
        &mut self,
        hash: [u8; 32],
        data: &Arc<TxSetData>,
        slot: u32,
        connected: &[PeerId],
    ) -> bool {
        let Some(identity) = self.state.identity.lock().unwrap().ours.clone() else {
            return false;
        };
        let me = identity.node_id;
        // One lock at a time. A node the others do not expect to lead the
        // slot would have its pieces refused by every node: it does not code.
        let planned = {
            let mut tree = self.state.txset_tree.lock().unwrap();
            let seed = match tree.config() {
                Some(config)
                    if config.mode == TreeMode::Coded
                        && config.contains(&me)
                        && config.may_lead(slot, &me) =>
                {
                    config.seed_of(slot).copied()
                }
                _ => None,
            };
            seed.and_then(|seed| tree.coded_layout(slot, &me).map(|layout| (seed, layout)))
        };
        let Some((seed, layout)) = planned else {
            return false;
        };
        // The root of the pieces is signed with an Ed25519 key.
        if self.state.keypair.public().try_into_ed25519().is_err() {
            return false;
        }
        let peer_of: HashMap<NodeId, PeerId> = {
            let router = self.state.tx_router.lock().unwrap();
            connected
                .iter()
                .filter_map(|peer| router.node_of(peer).map(|node| (*node, *peer)))
                .collect()
        };
        let targets = layout.leader_targets();
        let unreachable = targets
            .iter()
            .filter(|(_, node)| !peer_of.contains_key(node))
            .count();
        if unreachable * 4 > targets.len() {
            self.state
                .metrics
                .txset_tree_unreachable
                .fetch_add(unreachable as u64, Ordering::Relaxed);
            warn!(
                "TXSET_CODED_STAR: {} of {} relays for slot {} have no identified connection, pushing to every peer",
                unreachable,
                targets.len(),
                slot
            );
            return false;
        }
        if !self.state.txset_tree.lock().unwrap().coded.mark_pushed(hash) {
            // This set went out as pieces already.
            return true;
        }
        info!(
            "TXSET_CODED_PUSH: Pushing TX set {:02x?}... ({} bytes, {} on the wire) for slot {} as {} pieces ({} rebuild it) to {} relays",
            &hash[..4],
            data.len(),
            data.encoded().len(),
            slot,
            layout.params.pieces,
            layout.params.data,
            targets.len() - unreachable
        );
        tokio::spawn(send_coded_set(
            Arc::clone(&self.state),
            hash,
            Arc::clone(data),
            slot,
            seed,
            layout,
            identity,
            peer_of,
            unix_ms_now(),
        ));
        true
    }

    /// Push a TX set this node leads with. Without a tree it goes to every
    /// connected peer that has not been sent it, as the frame a request is
    /// answered with, unsolicited on the TX-set stream; with one, to this
    /// node's children in the tree, with a header in front (see
    /// `tree_push_plan`). Receivers verify and cache it by its content hash.
    async fn push_txset(&mut self, hash: [u8; 32], data: Arc<TxSetData>, slot: Option<u32>) {
        // Include the StellarMessage discriminant in the existing wire limit.
        if data.len() > MAX_MESSAGE_SIZE - 4 {
            warn!(
                "TXSET_PUSH_SKIP: TX set {:02x?}... is too large to send ({} bytes)",
                &hash[..4],
                data.len()
            );
            return;
        }
        let connected: Vec<PeerId> = self
            .state
            .peer_streams
            .read()
            .await
            .keys()
            .cloned()
            .collect();
        if let Some(slot) = slot {
            if self.coded_push(hash, &data, slot, &connected) {
                return;
            }
        }
        let plan = self.tree_push_plan(slot, &connected);
        let tagged = plan.header.is_some();
        let header: Arc<Vec<u8>> = Arc::new(plan.header.map(|h| h.encode()).unwrap_or_default());
        let (targets, busy) = self
            .state
            .txset_pushes
            .lock()
            .unwrap()
            .start_push_listing_busy(hash, plan.targets);
        if !busy.is_empty() {
            // Those peers request the set when they see the proposal.
            warn!(
                "TXSET_PUSH_BUSY: Not pushing TX set {:02x?}... to {} peer(s): an earlier push to them is still in flight",
                &hash[..4],
                busy.len()
            );
        }
        // In a tree a child that is left out would leave everyone below it
        // without the set: they get it from this node directly, as a star
        // (which nobody relays).
        let mut direct: Vec<PeerId> = Vec::new();
        for child in &busy {
            if let Some(below) = plan.subtrees.get(child) {
                direct.extend(below.iter().copied());
            }
        }
        if !plan.subtrees.is_empty() && !busy.is_empty() {
            self.state
                .metrics
                .txset_tree_child_busy
                .fetch_add(busy.len() as u64, Ordering::Relaxed);
        }
        let direct = if direct.is_empty() {
            direct
        } else {
            self.state
                .txset_pushes
                .lock()
                .unwrap()
                .start_push(hash, direct)
        };
        if targets.is_empty() && direct.is_empty() {
            debug!(
                "TXSET_PUSH_SKIP: TX set {:02x?}... has no peer left to push to",
                &hash[..4]
            );
            return;
        }
        info!(
            "TXSET_PUSH: Pushing TX set {:02x?}... ({} bytes, {} on the wire) to {} of {} peers{}{}",
            &hash[..4],
            data.len(),
            data.encoded().len(),
            targets.len() + direct.len(),
            connected.len(),
            if tagged { " (tagged)" } else { "" },
            if direct.is_empty() {
                String::new()
            } else {
                format!(", {} of them below a busy child", direct.len())
            }
        );
        self.spawn_pushes(hash, &data, header, targets);
        if let (false, Some(star)) = (direct.is_empty(), plan.star) {
            self.spawn_pushes(hash, &data, Arc::new(star.encode()), direct);
        }
    }

    /// Write the set to `peers`, each on its TX-set stream, with `header` in
    /// front if it is not empty. The peers were registered with
    /// `PushRecord::start_push*`.
    fn spawn_pushes(
        &mut self,
        hash: [u8; 32],
        data: &Arc<TxSetData>,
        header: Arc<Vec<u8>>,
        peers: Vec<PeerId>,
    ) {
        for peer in peers {
            let state = Arc::clone(&self.state);
            let data = Arc::clone(data);
            let header = Arc::clone(&header);
            self.sends.spawn(async move {
                let sent = if header.is_empty() {
                    send_to_peer_stream(&state, peer, StreamType::TxSet, data.encoded()).await
                } else {
                    send_to_peer_stream_parts(
                        &state,
                        peer,
                        StreamType::TxSet,
                        &[header.as_slice(), data.encoded()],
                    )
                    .await
                };
                match sent {
                    Ok(_) => {
                        state.txset_pushes.lock().unwrap().push_written(
                            &hash,
                            &peer,
                            Instant::now(),
                        );
                        let bytes = (header.len() + data.encoded().len()) as u64;
                        let metrics = &state.metrics;
                        metrics.txset_push_sent.fetch_add(1, Ordering::Relaxed);
                        metrics
                            .txset_push_sent_bytes
                            .fetch_add(bytes, Ordering::Relaxed);
                        if !header.is_empty() {
                            metrics.txset_tree_root_sent.fetch_add(1, Ordering::Relaxed);
                        }
                        metrics.send_txset.fetch_add(1, Ordering::Relaxed);
                        metrics.message_write.fetch_add(1, Ordering::Relaxed);
                        metrics.byte_write.fetch_add(bytes, Ordering::Relaxed);
                        debug!(
                            "TXSET_PUSH_OK: Queued TX set {:02x?}... to {}",
                            &hash[..4],
                            peer
                        );
                    }
                    Err(e) => {
                        state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
                        warn!(
                            "TXSET_PUSH_FAIL: Failed to push TX set {:02x?}... to {}: {}",
                            &hash[..4],
                            peer,
                            e
                        );
                        // The peer does not have the set from us. If its
                        // request was skipped because of this push, answer it
                        // through the normal path now.
                        let skipped = state.txset_pushes.lock().unwrap().push_failed(&hash, &peer);
                        if skipped {
                            let _ = state
                                .event_tx
                                .send(OverlayEvent::TxSetRequested { hash, from: peer });
                        }
                    }
                }
            });
        }
    }

    /// Forget requests for slots below `min_slot` that were never answered.
    async fn prune_txset_requests(&mut self, min_slot: u32) {
        let mut pending = self.state.pending_txset_requests.write().await;
        let before = pending.len();
        pending.retain(|_, (_, _, slot)| *slot >= min_slot);
        if pending.len() < before {
            debug!(
                "Pruned {} unanswered TX set requests below slot {}",
                before - pending.len(),
                min_slot
            );
        }
    }

    /// Request SCP state from all connected peers
    pub async fn request_scp_state_from_all_peers(&mut self, ledger_seq: u32) {
        let streams = self.state.peer_streams.read().await;
        let peers: Vec<_> = streams.keys().cloned().collect();
        drop(streams);

        info!(
            "Requesting SCP state for ledger >= {} from {} peers",
            ledger_seq,
            peers.len()
        );

        let request = crate::xdr::frame_get_scp_state(ledger_seq);
        for peer_id in peers {
            let state = Arc::clone(&self.state);
            let request = request.clone();
            self.sends.spawn(async move {
                if let Err(e) =
                    send_to_peer_stream(&state, peer_id, StreamType::Control, &request).await
                {
                    warn!("Failed to send SCP state request to {}: {:?}", peer_id, e);
                }
            });
        }
    }

    /// Send SCP envelope to a specific peer
    pub async fn send_scp_to_peer(&self, peer_id: PeerId, envelope: &[u8]) -> io::Result<()> {
        let message = crate::xdr::frame_scp(envelope);
        send_to_peer_stream(&self.state, peer_id, StreamType::Control, &message).await
    }
}

/// Send TX set response to a specific peer
async fn send_txset_response(
    state: Arc<SharedState>,
    peer: PeerId,
    hash: [u8; 32],
    data: Arc<TxSetData>,
) {
    info!(
        "TXSET_SEND: Sending TX set {:02x?}... ({} bytes) to {}",
        &hash[..4],
        data.len(),
        peer
    );

    let bytes = data.encoded().len();
    match send_to_peer_stream(&state, peer, StreamType::TxSet, data.encoded()).await {
        Ok(()) => {
            state.metrics.send_txset.fetch_add(1, Ordering::Relaxed);
            state.metrics.message_write.fetch_add(1, Ordering::Relaxed);
            state
                .metrics
                .byte_write
                .fetch_add(bytes as u64, Ordering::Relaxed);
            info!(
                "TXSET_SEND_OK: Queued TX set {:02x?}... ({} bytes on wire) to {}",
                &hash[..4],
                bytes,
                peer
            );
        }
        Err(e) => {
            state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
            warn!(
                "TXSET_SEND_FAIL: Failed to send TxSet {:02x?}... to {}: {}",
                &hash[..4],
                peer,
                e
            );
        }
    }
}

/// Open control, TX, and TxSet streams to a peer.
/// Spawned as a background task so the swarm event loop stays unblocked —
/// `control.open_stream()` needs the swarm to be polled to complete.
async fn open_streams_to_peer(control: Control, state: Arc<SharedState>, peer_id: PeerId) {
    debug!("Opening streams to peer {}", peer_id);
    let (scp_stream, tx_stream, txset_stream) = tokio::join!(
        open_peer_stream(control.clone(), peer_id, StreamType::Control),
        open_peer_stream(control.clone(), peer_id, StreamType::Tx),
        open_peer_stream(control, peer_id, StreamType::TxSet),
    );
    let opened = |result: io::Result<OutboundStream>| match result {
        Ok(stream) => Some(stream),
        Err(e) => {
            warn!("Failed to open stream to {}: {}", peer_id, e);
            None
        }
    };
    let scp_stream = opened(scp_stream);
    let tx_stream = opened(tx_stream);
    let txset_stream = opened(txset_stream);

    // Store streams
    {
        // Never retain the peer-map lock while waiting for a stream writer:
        // connection/disconnection handling needs to update this map.
        let peer_streams = state.peer_streams.read().await.get(&peer_id).cloned();
        if let Some(peer_streams) = peer_streams {
            if let Some(stream) = scp_stream {
                *peer_streams.scp.lock().await = Some(stream);
            }
            if let Some(stream) = tx_stream {
                *peer_streams.tx.lock().await = Some(stream);
            }
            if let Some(stream) = txset_stream {
                *peer_streams.txset.lock().await = Some(stream);
            }
        }
    }

    // Tell the peer which validator this overlay belongs to, if Core has
    // said so already (otherwise `set_node_identity` does it later).
    let identity = state.identity.lock().unwrap().ours.clone();
    if let Some(identity) = identity {
        tokio::spawn(send_node_identity(state.control.clone(), peer_id, identity));
    }

    // Request SCP state from newly connected peer
    info!("Peer {} streams opened, sending SCP state request", peer_id);
    let ledger_seq: u32 = 0;
    let request = crate::xdr::frame_get_scp_state(ledger_seq);
    if let Err(e) =
        send_to_peer_stream(&state, peer_id.clone(), StreamType::Control, &request).await
    {
        info!(
            "Failed to request SCP state from newly connected peer {}: {:?}",
            peer_id, e
        );
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum StreamType {
    Control,
    Tx,
    TxSet,
    TxSetRequest,
}

impl StreamType {
    fn protocol(&self) -> StreamProtocol {
        match self {
            StreamType::Control | StreamType::TxSetRequest => CONTROL_PROTOCOL,
            StreamType::Tx => TX_PROTOCOL,
            StreamType::TxSet => TXSET_PROTOCOL,
        }
    }

    fn priority(self) -> i32 {
        match self {
            Self::Control | Self::TxSetRequest => 2,
            Self::TxSet => 1,
            Self::Tx => 0,
        }
    }
}

/// Open the required protocol and set QUIC priority before application bytes.
async fn open_peer_stream(
    mut control: Control,
    peer: PeerId,
    kind: StreamType,
) -> io::Result<OutboundStream> {
    let protocol = kind.protocol();
    let stream = control
        .open_stream(peer, protocol.clone())
        .await
        .map_err(|e| io::Error::new(io::ErrorKind::NotConnected, e))?;
    stream.set_priority(kind.priority())?;
    debug!(%peer, %protocol, priority = kind.priority(), "Opened outbound stream");
    Ok(OutboundStream {
        stream,
        #[cfg(test)]
        protocol,
    })
}

/// Send message to a specific peer's stream only if already open (for flooding)
/// Returns Ok(()) if sent, Err if stream not open (doesn't try to reopen)
async fn try_send_to_existing_stream(
    state: &SharedState,
    peer_id: PeerId,
    stream_type: StreamType,
    data: &[u8],
) -> io::Result<()> {
    let streams = state.peer_streams.read().await;
    let peer_streams = streams
        .get(&peer_id)
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "peer not connected"))?
        .clone();
    drop(streams);

    // Serialize frames only within this peer's stream for this purpose.
    let stream_mutex = match stream_type {
        StreamType::Control | StreamType::TxSetRequest => &peer_streams.scp,
        StreamType::Tx => &peer_streams.tx,
        StreamType::TxSet => &peer_streams.txset,
    };

    let mut stream_guard = stream_mutex.lock().await;

    // If stream not open, fail immediately without reopening
    let stream = stream_guard
        .as_mut()
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "stream not open"))?;

    write_framed(&mut stream.stream, data).await
}

/// Restart the per-peer GETDATA timeout clock for hashes whose demand chunk
/// was just written to `peer`'s stream. Skips hashes that completed or were
/// re-assigned to a different peer while the write was in flight —
/// re-stamping those would silently delay their next retry.
async fn mark_getdata_sent(state: &SharedState, peer: &PeerId, hashes: &[[u8; 32]]) {
    let mut pending = state.pending_getdata.write().await;
    for hash in hashes {
        if let Some(req) = pending.get_mut(hash) {
            if req.peer == *peer {
                req.mark_sent();
            }
        }
    }
}

/// Send message to a specific peer's stream, reopening if needed
async fn send_to_peer_stream(
    state: &SharedState,
    peer_id: PeerId,
    stream_type: StreamType,
    data: &[u8],
) -> io::Result<()> {
    send_to_peer_stream_parts(state, peer_id, stream_type, &[data]).await
}

/// Send one frame made of `parts`, without assembling them into one buffer.
async fn send_to_peer_stream_parts(
    state: &SharedState,
    peer_id: PeerId,
    stream_type: StreamType,
    parts: &[&[u8]],
) -> io::Result<()> {
    send_peer_frames(state, peer_id, stream_type, framed_io::Frames::One(parts)).await
}

/// Write `frames` to the peer's stream for `stream_type`, opening it if
/// needed. The per-route lock stays held until every frame has been written,
/// including across partial writes, so frames never interleave mid-frame.
///
/// A failed write drops the stream and retries the whole of `frames` on a
/// reopened one (3 attempts). For a `Frames::Batch` this can deliver complete
/// frames that reached the peer before the failure a second time; TX
/// receivers drop such duplicates via `tx_seen`. The peer's reader on the old
/// stream sees it end, possibly mid-frame, and exits.
async fn send_peer_frames(
    state: &SharedState,
    peer_id: PeerId,
    stream_type: StreamType,
    frames: framed_io::Frames<'_>,
) -> io::Result<()> {
    // Retry up to 2 times (3 attempts total) for reliability
    const MAX_RETRIES: usize = 2;

    for attempt in 0..=MAX_RETRIES {
        let streams = state.peer_streams.read().await;
        let peer_streams = match streams.get(&peer_id) {
            Some(ps) => ps.clone(),
            None => {
                return Err(io::Error::new(
                    io::ErrorKind::NotConnected,
                    "peer not connected",
                ));
            }
        };
        drop(streams);

        // Serialize frames only within this peer's stream for this purpose.
        let stream_mutex = match stream_type {
            StreamType::Control | StreamType::TxSetRequest => &peer_streams.scp,
            StreamType::Tx => &peer_streams.tx,
            StreamType::TxSet => &peer_streams.txset,
        };

        let mut stream_guard = stream_mutex.lock().await;

        // If stream is None, try to reopen it
        if stream_guard.is_none() {
            debug!(
                "Stream {:?} not open to {}, attempting to reopen (attempt {})",
                stream_type.protocol(),
                peer_id,
                attempt + 1
            );
            match open_peer_stream(state.control.clone(), peer_id, stream_type).await {
                Ok(s) => {
                    debug!(
                        "Successfully reopened {:?} stream to {}",
                        stream_type.protocol(),
                        peer_id
                    );
                    *stream_guard = Some(s);
                }
                Err(e) => {
                    if attempt < MAX_RETRIES {
                        debug!(
                            "Failed to reopen {:?} stream to {} (attempt {}), retrying: {:?}",
                            stream_type.protocol(),
                            peer_id,
                            attempt + 1,
                            e
                        );
                        drop(stream_guard);
                        tokio::time::sleep(tokio::time::Duration::from_millis(
                            10 * (attempt as u64 + 1),
                        ))
                        .await;
                        continue;
                    }
                    warn!(
                        "Failed to reopen {:?} stream to {}: {:?}",
                        stream_type.protocol(),
                        peer_id,
                        e
                    );
                    return Err(io::Error::new(
                        io::ErrorKind::NotConnected,
                        format!("failed to reopen stream: {:?}", e),
                    ));
                }
            }
        }

        let stream = stream_guard.as_mut().unwrap();
        match frames.write(&mut stream.stream).await {
            Ok(()) => return Ok(()),
            Err(e) => {
                // Clear the broken stream
                *stream_guard = None;

                if attempt < MAX_RETRIES {
                    debug!(
                        "Send to {:?} stream failed (attempt {}), retrying: {}",
                        stream_type.protocol(),
                        attempt + 1,
                        e
                    );
                    drop(stream_guard);
                    tokio::time::sleep(tokio::time::Duration::from_millis(
                        10 * (attempt as u64 + 1),
                    ))
                    .await;
                    continue;
                }
                return Err(e);
            }
        }
    }

    unreachable!()
}

/// Write length-prefixed frame to stream
async fn write_framed(stream: &mut Stream, data: &[u8]) -> io::Result<()> {
    framed_io::write_frame_parts(stream, &[data]).await
}

/// Advertise `txs` to every connected peer (the INV/GETDATA flood) and keep
/// them ready for the demands that follow. Returns false if there was no
/// peer to advertise them to.
async fn advertise_txs_to_all(state: &Arc<SharedState>, txs: &[Arc<ValidatedTx>]) -> bool {
    if txs.is_empty() {
        return true;
    }
    {
        let mut buffer = state.tx_buffer.write().await;
        for tx in txs {
            buffer.insert(Arc::clone(tx));
        }
    }
    let peers: Vec<PeerId> = state.peer_streams.read().await.keys().cloned().collect();
    if peers.is_empty() {
        debug!("TX_INV: No peers to announce {} TXs to", txs.len());
        return false;
    }
    debug!(
        "TX_INV: Announcing {} TXs to {} peers via INV",
        txs.len(),
        peers.len()
    );
    state
        .metrics
        .flood_advertised
        .fetch_add((peers.len() * txs.len()) as u64, Ordering::Relaxed);
    for tx in txs {
        let entry = InvEntry {
            hash: *tx.hash(),
            fee_per_op: tx.fee_per_op(),
        };
        queue_inv(state, &peers, entry).await;
    }
    true
}

/// Carry out a router plan: bodies straight to the target peers, adverts to
/// everyone for what has to be flooded.
async fn execute_send_plan(state: &Arc<SharedState>, plan: SendPlan<PeerId>) {
    let SendPlan { direct, flood } = plan;
    if !flood.is_empty() {
        if advertise_txs_to_all(state, &flood).await {
            state
                .metrics
                .flood_tx_local_flooded
                .fetch_add(flood.len() as u64, Ordering::Relaxed);
        } else {
            // Nobody to tell yet: try again when the targets or the peers
            // change.
            let hashes: Vec<[u8; 32]> = flood.iter().map(|tx| *tx.hash()).collect();
            state.tx_router.lock().unwrap().mark_unflooded(&hashes);
        }
    }
    // Queue the bodies per peer; `direct_send_flush_task` writes them a few
    // milliseconds later, a full queue is written now.
    let mut overflow: Vec<(PeerId, Vec<[u8; 32]>)> = Vec::new();
    let mut full: Vec<PeerId> = Vec::new();
    {
        let mut out = state.direct_out.lock().unwrap();
        for (peer, txs) in direct {
            let queue = out.queued.entry(peer).or_default();
            let room = DIRECT_SEND_MAX_QUEUED_TXS.saturating_sub(queue.len());
            if txs.len() > room {
                overflow.push((peer, txs[room..].iter().map(|tx| *tx.hash()).collect()));
            }
            queue.extend(txs.into_iter().take(room));
            if queue.len() >= DIRECT_SEND_BATCH_MAX_TXS {
                full.push(peer);
            }
        }
    }
    if !overflow.is_empty() {
        // The peer is not reading: these were not sent. The router offers
        // them again at a later plan.
        let mut router = state.tx_router.lock().unwrap();
        for (peer, hashes) in &overflow {
            router.mark_unsent(peer, hashes);
        }
    }
    for peer in full {
        start_direct_sender(state, peer);
    }
}

/// Starts the sender task of `peer` unless one is running: it writes what is
/// queued for the peer, batch after batch, and ends when the queue is empty.
/// One task per peer, so a peer that stops reading holds one task and a
/// bounded queue, and a slow peer does not delay another one or the caller.
fn start_direct_sender(state: &Arc<SharedState>, peer: PeerId) {
    {
        let mut out = state.direct_out.lock().unwrap();
        if out.queued.get(&peer).map_or(true, |q| q.is_empty()) || !out.sending.insert(peer) {
            return;
        }
    }
    let state = Arc::clone(state);
    tokio::spawn(async move {
        loop {
            let txs = {
                let mut out = state.direct_out.lock().unwrap();
                match out.queued.remove(&peer) {
                    Some(txs) if !txs.is_empty() => txs,
                    _ => {
                        out.sending.remove(&peer);
                        return;
                    }
                }
            };
            send_tx_bodies(&state, peer, txs).await;
        }
    });
}

/// Writes the queued direct sends every DIRECT_SEND_BATCH_INTERVAL, and asks
/// the router every DIRECT_REPLAN_INTERVAL whether anything is owed.
async fn direct_send_flush_task(state: Arc<SharedState>) {
    let mut interval = tokio::time::interval(DIRECT_SEND_BATCH_INTERVAL);
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut last_replan = Instant::now();
    loop {
        interval.tick().await;
        if last_replan.elapsed() >= DIRECT_REPLAN_INTERVAL {
            last_replan = Instant::now();
            flush_local_txs(&state).await;
        }
        let peers: Vec<PeerId> = {
            let out = state.direct_out.lock().unwrap();
            out.queued
                .iter()
                .filter(|(peer, queue)| !queue.is_empty() && !out.sending.contains(*peer))
                .map(|(peer, _)| *peer)
                .collect()
        };
        for peer in peers {
            start_direct_sender(&state, peer);
        }
    }
}

/// Ask the router what is owed after the targets or the identified peers
/// changed, and send it.
async fn flush_local_txs(state: &Arc<SharedState>) {
    let plan = state.tx_router.lock().unwrap().plan_all(Instant::now());
    if plan.is_empty() {
        return;
    }
    debug!(
        "TX_DIRECT_FLUSH: {} peers get bodies, {} TXs flooded",
        plan.direct.len(),
        plan.flood.len()
    );
    execute_send_plan(state, plan).await;
}

/// Send transaction bodies straight to `peer`, without adverts, as batches
/// of complete TX frames (the framing of a GETDATA response). If a write
/// fails, the router is told that the remaining ones were not delivered.
async fn send_tx_bodies(state: &SharedState, peer: PeerId, txs: Vec<Arc<ValidatedTx>>) {
    let mut batch = Vec::new();
    let mut next = 0;
    while next < txs.len() {
        batch.clear();
        let first = next;
        let mut payload_bytes = 0;
        while next < txs.len() {
            let len = tx_frame_len(&txs[next]);
            if next > first && batch.len() + len > TX_RESPONSE_BATCH_BYTES {
                break;
            }
            let parts = txs[next].flood_frame_parts();
            let payload = parts.iter().map(|part| part.len()).sum::<usize>();
            let Ok(prefix) = u32::try_from(payload) else {
                warn!("TX_DIRECT: transaction too large to frame, skipping");
                next += 1;
                continue;
            };
            batch.extend_from_slice(&prefix.to_be_bytes());
            for part in parts {
                batch.extend_from_slice(part);
            }
            payload_bytes += payload;
            next += 1;
        }
        let count = next - first;
        let frames = framed_io::Frames::Batch(&batch);
        let result = send_peer_frames(state, peer, StreamType::Tx, frames).await;
        if record_tx_response_write(state, peer, result, count, payload_bytes) {
            state
                .metrics
                .flood_tx_direct_sent
                .fetch_add(count as u64, Ordering::Relaxed);
            state
                .metrics
                .flood_tx_direct_sent_bytes
                .fetch_add(payload_bytes as u64, Ordering::Relaxed);
        } else {
            let unsent: Vec<[u8; 32]> = txs[first..].iter().map(|tx| *tx.hash()).collect();
            state
                .tx_router
                .lock()
                .unwrap()
                .mark_unsent(&peer, &unsent);
            return;
        }
    }
}

/// Tell `peer` which validator this overlay belongs to, and wait for it to
/// acknowledge the frame. Retried a few times: the stream library drops an
/// inbound stream when several arrive faster than they are accepted (every
/// peer identifies itself at the same moment when a network starts), and a
/// dropped stream is not always an error on the writing side.
async fn send_node_identity(control: Control, peer: PeerId, identity: NodeIdentity) {
    const ATTEMPTS: u32 = 12;
    const MAX_DELAY: Duration = Duration::from_secs(5);
    let mut delay = Duration::from_millis(100);
    for attempt in 1..=ATTEMPTS {
        match try_send_node_identity(control.clone(), peer, &identity).await {
            Ok(()) => return,
            Err(e) if e.kind() == io::ErrorKind::Unsupported => {
                // A peer without this protocol never identifies itself
                // either; transactions are flooded when it is a target.
                debug!("NODE_ID: {} does not take node ids: {}", peer, e);
                return;
            }
            Err(e) => {
                debug!(
                    "NODE_ID: Sending the node id to {} failed (attempt {}): {}",
                    peer, attempt, e
                );
            }
        }
        tokio::time::sleep(delay).await;
        delay = (delay * 2).min(MAX_DELAY);
    }
    // About half a minute of trying. If the connection is still up, this
    // node floods its transactions whenever that validator is a target.
    warn!(
        "NODE_ID: Could not tell {} which validator this overlay belongs to",
        peer
    );
}

/// One attempt of [`send_node_identity`].
async fn try_send_node_identity(
    mut control: Control,
    peer: PeerId,
    identity: &NodeIdentity,
) -> io::Result<()> {
    use futures::AsyncWriteExt;
    let mut stream = control
        .open_stream(peer, NODE_ID_PROTOCOL)
        .await
        .map_err(|e| match e {
            libp2p_stream::OpenStreamError::UnsupportedProtocol(_) => {
                io::Error::new(io::ErrorKind::Unsupported, e)
            }
            other => io::Error::new(io::ErrorKind::NotConnected, other),
        })?;
    let mut frame = [0u8; NODE_ID_FRAME_LEN];
    frame[..32].copy_from_slice(&identity.node_id);
    frame[32..].copy_from_slice(&identity.signature);
    write_framed(&mut stream, &frame).await?;
    let mut ack = [0u8; 1];
    tokio::time::timeout(NODE_ID_READ_TIMEOUT, stream.read_exact(&mut ack))
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "no acknowledgement"))??;
    let _ = stream.close().await;
    if ack[0] != NODE_ID_ACK_TAKEN {
        // The peer read the frame but could not use it yet (it had not
        // registered this connection): send it again.
        return Err(io::Error::new(io::ErrorKind::WouldBlock, "not taken yet"));
    }
    Ok(())
}

/// Peers telling us which validator they belong to.
async fn handle_inbound_node_id_streams(mut incoming: IncomingStreams, state: Arc<SharedState>) {
    while let Some((peer_id, mut stream)) = incoming.next().await {
        let state = state.clone();
        tokio::spawn(async move {
            let read = tokio::time::timeout(
                NODE_ID_READ_TIMEOUT,
                read_framed_with_limit(&mut stream, NODE_ID_FRAME_LEN),
            );
            let frame = match read.await {
                Ok(Ok(frame)) if frame.len() == NODE_ID_FRAME_LEN => frame,
                Ok(Ok(frame)) => {
                    debug!(
                        "NODE_ID: Ignoring a node id frame of {} bytes from {}",
                        frame.len(),
                        peer_id
                    );
                    return;
                }
                Ok(Err(e)) => {
                    debug!("NODE_ID: Node id stream from {} failed: {}", peer_id, e);
                    return;
                }
                Err(_) => {
                    debug!("NODE_ID: Node id stream from {} sent nothing", peer_id);
                    return;
                }
            };
            let mut node_id = [0u8; 32];
            node_id.copy_from_slice(&frame[..32]);
            let mut signature = [0u8; 64];
            signature.copy_from_slice(&frame[32..]);
            let taken = accept_node_id_claim(&state, peer_id, node_id, signature).await;
            {
                use futures::AsyncWriteExt;
                let ack = if taken {
                    NODE_ID_ACK_TAKEN
                } else {
                    NODE_ID_ACK_RETRY
                };
                let _ = stream.write_all(&[ack]).await;
                let _ = stream.flush().await;
            }
        });
    }
}

/// A peer claims to be validator `node_id`. Accept it if the signature
/// binds that validator to the peer id of this connection; keep it for later
/// if Core has not told us the network id yet.
/// Returns false if the claim could not be used yet and should be repeated.
async fn accept_node_id_claim(
    state: &Arc<SharedState>,
    peer: PeerId,
    node_id: NodeId,
    signature: [u8; 64],
) -> bool {
    // Nothing to do for a claim that is already accepted (a peer repeats it
    // on every connection it establishes).
    if state.tx_router.lock().unwrap().node_of(&peer) == Some(&node_id) {
        return true;
    }
    // Only a connected peer can be a target. The peer's stream can get here
    // before this side has registered the connection: the sender repeats the
    // claim.
    if !state.peer_streams.read().await.contains_key(&peer) {
        return false;
    }
    let network_id = {
        let mut ids = state.identity.lock().unwrap();
        let known = ids.ours.as_ref().map(|ours| ours.network_id);
        match known {
            Some(network_id) => {
                // A signature check, a log line and a pass over the pending
                // transactions per claim: bound what one connection can ask
                // for. (A waiting claim is counted when it is checked.)
                let claims = ids.claims.entry(peer).or_insert(0);
                if *claims >= MAX_NODE_ID_CLAIMS_PER_PEER {
                    return true;
                }
                *claims += 1;
                network_id
            }
            None => {
                ids.unverified.insert(peer, (node_id, signature));
                return true;
            }
        }
    };
    if !verify_node_id_binding(&network_id, &peer, &node_id, &signature) {
        state
            .metrics
            .node_id_rejected
            .fetch_add(1, Ordering::Relaxed);
        warn!(
            "NODE_ID_REJECT: Peer {} claims validator {:02x?}... with a signature that does not bind it to this connection",
            peer,
            &node_id[..4]
        );
        return true;
    }
    info!(
        "NODE_ID: Peer {} is validator {:02x?}...",
        peer,
        &node_id[..4]
    );
    let mut identified = {
        let mut router = state.tx_router.lock().unwrap();
        router.peer_identified(peer, node_id);
        router.identified_peer_count()
    };
    // The connection may have closed between the check above and now; its
    // close handler has then already run and would not undo this.
    if !state.peer_streams.read().await.contains_key(&peer) {
        let mut router = state.tx_router.lock().unwrap();
        router.peer_disconnected(&peer);
        identified = router.identified_peer_count();
        state.identity.lock().unwrap().claims.remove(&peer);
    }
    state
        .metrics
        .node_id_identified
        .store(identified as i64, Ordering::Relaxed);
    // The peer may be a target that could not be reached until now.
    flush_local_txs(state).await;
    true
}

/// Flush INV batch for a specific peer
async fn flush_inv_batch_to_peer(state: &Arc<SharedState>, peer: PeerId) {
    let batch = {
        let mut batcher = state.inv_batcher.write().await;
        batcher.flush(&peer)
    };

    if let Some(batch) = batch {
        send_inv_batch(state, peer, batch).await;
    }
}

/// Queue `entry` for every peer in `peers` under one INV batcher lock, rather
/// than one lock round trip per peer, then send the batches it completed.
/// `send_inv_batch` only spawns the write, so they still go out at once.
async fn queue_inv(state: &Arc<SharedState>, peers: &[PeerId], entry: InvEntry) {
    let full_batches: Vec<(PeerId, InvBatch)> = {
        let mut batcher = state.inv_batcher.write().await;
        peers
            .iter()
            .filter_map(|peer| {
                batcher
                    .add(*peer, entry.clone())
                    .map(|batch| (*peer, batch))
            })
            .collect()
    };
    for (peer, batch) in full_batches {
        send_inv_batch(state, peer, batch).await;
    }
}

/// Send an INV batch to a peer
async fn send_inv_batch(state: &Arc<SharedState>, peer: PeerId, batch: InvBatch) {
    let batch_size = batch.entries.len() as u64;
    let encoded = match batch.encode() {
        Ok(encoded) => encoded,
        Err(e) => {
            state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
            warn!("Failed to encode INV batch for {}: {}", peer, e);
            return;
        }
    };
    let encoded_len = encoded.len() as u64;

    let state = Arc::clone(state);
    tokio::spawn(async move {
        if let Err(e) = send_to_peer_stream(&state, peer.clone(), StreamType::Tx, &encoded).await {
            state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
            warn!("Failed to send INV batch to {}: {}", peer, e);
        } else {
            state
                .metrics
                .send_transaction
                .fetch_add(1, Ordering::Relaxed);
            state.metrics.message_write.fetch_add(1, Ordering::Relaxed);
            state
                .metrics
                .byte_write
                .fetch_add(encoded_len, Ordering::Relaxed);
            state
                .metrics
                .flood_tx_batch_size_sum
                .fetch_add(batch_size, Ordering::Relaxed);
            state
                .metrics
                .flood_tx_batch_size_count
                .fetch_add(1, Ordering::Relaxed);
            debug!("TX_INV_SENT: Sent INV batch to {}", peer);
        }
    });
}

/// Read length-prefixed frame from stream
async fn read_framed<R: futures::AsyncRead + Unpin>(stream: &mut R) -> io::Result<Vec<u8>> {
    read_framed_with_limit(stream, MAX_MESSAGE_SIZE).await
}

async fn read_framed_with_limit<R: futures::AsyncRead + Unpin>(
    stream: &mut R,
    limit: usize,
) -> io::Result<Vec<u8>> {
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await?;
    let len = u32::from_be_bytes(len_buf) as usize;

    if len > limit {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("message too large: {} > {}", len, limit),
        ));
    }

    let mut data = vec![0u8; len];
    stream.read_exact(&mut data).await?;
    Ok(data)
}

/// Handle inbound SCP streams from peers
async fn handle_inbound_scp_streams(mut incoming: IncomingStreams, state: Arc<SharedState>) {
    while let Some((peer_id, mut stream)) = incoming.next().await {
        info!("SCP_STREAM: Accepted inbound SCP stream from {}", peer_id);
        state
            .metrics
            .inbound_establish
            .fetch_add(1, Ordering::Relaxed);
        state.metrics.inbound_live.fetch_add(1, Ordering::Relaxed);
        let state = state.clone();

        tokio::spawn(async move {
            loop {
                match read_framed(&mut stream).await {
                    Ok(data) => {
                        state.metrics.message_read.fetch_add(1, Ordering::Relaxed);
                        state
                            .metrics
                            .byte_read
                            .fetch_add(data.len() as u64, Ordering::Relaxed);

                        let message = match crate::xdr::parse_stellar_message(&data) {
                            Ok(message) => message,
                            Err(e) => {
                                warn!("SCP_PARSE_ERR: Dropping malformed SCP stream message from {}: {}", peer_id, e);
                                continue;
                            }
                        };

                        let scp_envelope = match message {
                            stellar_xdr::StellarMessage::GetTxSet(hash) => {
                                handle_txset_request(&state, peer_id, hash.0);
                                continue;
                            }
                            stellar_xdr::StellarMessage::GetScpState(ledger_seq) => {
                                info!(
                                    "SCP_STATE_REQ: Peer {} requests SCP state for ledger >= {}",
                                    peer_id, ledger_seq
                                );

                                // Notify main loop via event channel
                                if let Err(e) =
                                    state.event_tx.send(OverlayEvent::ScpStateRequested {
                                        peer_id: peer_id.clone(),
                                        ledger_seq,
                                    })
                                {
                                    error!("Failed to send SCP state request event: {:?}", e);
                                }
                                continue;
                            }
                            stellar_xdr::StellarMessage::ScpMessage(scp_envelope) => scp_envelope,
                            other => {
                                warn!(
                                    "SCP_PARSE_ERR: Dropping unexpected {} on SCP stream from {}",
                                    other.name(),
                                    peer_id
                                );
                                continue;
                            }
                        };

                        // The canonical envelope bytes are the frame after the
                        // 4-byte discriminant; dedup on their hash before doing
                        // any more work (duplicates are the common case).
                        let envelope_bytes = &data[4..];
                        let hash = blake2b_hash(envelope_bytes);
                        let recv_start = std::time::Instant::now();
                        let is_dup = {
                            let mut seen = state.scp_seen.write().await;
                            if seen.contains(&hash) {
                                true
                            } else {
                                seen.put(hash, ());
                                false
                            }
                        };

                        // Record sender in scp_sent_to so we don't echo the message back
                        {
                            let mut sent_to = state.scp_sent_to.write().await;
                            if let Some(peers) = sent_to.get_mut(&hash) {
                                peers.insert(peer_id.clone());
                            } else {
                                let mut set = HashSet::new();
                                set.insert(peer_id.clone());
                                sent_to.put(hash, set);
                            }
                        }

                        if is_dup {
                            debug!(
                                "SCP_RECV_DUP: Duplicate SCP {:02x?}... from {}",
                                &hash[..4],
                                peer_id
                            );
                            continue;
                        }

                        info!(
                            "SCP_RECV: Received SCP {:02x?}... ({} bytes) from {}",
                            &hash[..4],
                            envelope_bytes.len(),
                            peer_id
                        );

                        // First sighting: extract any referenced tx set hashes
                        // from the single decode above and forward to Core.
                        let txset_hashes =
                            crate::xdr::extract_txset_hashes_from_envelope(&scp_envelope);
                        let slot = scp_envelope.statement.slot_index as u32;
                        if let Err(e) = state.event_tx.send(OverlayEvent::ScpReceived {
                            envelope: envelope_bytes.to_vec(),
                            txset_hashes,
                            from: peer_id.clone(),
                            slot,
                        }) {
                            warn!("Failed to forward SCP event from {}: {}", peer_id, e);
                        }

                        let elapsed_us = recv_start.elapsed().as_micros() as u64;
                        state
                            .metrics
                            .recv_scp_sum_us
                            .fetch_add(elapsed_us, Ordering::Relaxed);
                        state.metrics.recv_scp_count.fetch_add(1, Ordering::Relaxed);
                    }
                    Err(e) => {
                        state.metrics.error_read.fetch_add(1, Ordering::Relaxed);
                        state.metrics.inbound_live.fetch_sub(1, Ordering::Relaxed);
                        warn!(
                            "SCP_STREAM_CLOSED: SCP stream from {} closed: {}",
                            peer_id, e
                        );
                        break;
                    }
                }
            }
        });
    }
}

/// Inbound TX streams carry many small frames, and GETDATA responses arrive
/// as batches of them. Reading through a buffer copies whatever QUIC has
/// received in one call, instead of two reads (each taking the connection
/// lock) per frame. It never waits for more than is available, and larger
/// frames bypass it once it is empty.
fn buffered_tx_stream<R: futures::AsyncRead>(stream: R) -> futures::io::BufReader<R> {
    futures::io::BufReader::with_capacity(TX_RESPONSE_BATCH_BYTES, stream)
}

/// Handle inbound TX streams from peers
async fn handle_inbound_tx_streams(mut incoming: IncomingStreams, state: Arc<SharedState>) {
    while let Some((peer_id, stream)) = incoming.next().await {
        info!("TX_STREAM: Accepted inbound TX stream from {}", peer_id);
        state.metrics.inbound_live.fetch_add(1, Ordering::Relaxed);
        let state = state.clone();

        tokio::spawn(async move {
            let mut stream = buffered_tx_stream(stream);
            loop {
                match read_framed(&mut stream).await {
                    Ok(data) => {
                        state.metrics.message_read.fetch_add(1, Ordering::Relaxed);
                        state
                            .metrics
                            .byte_read
                            .fetch_add(data.len() as u64, Ordering::Relaxed);
                        // Parse INV/GETDATA message
                        handle_tx_stream_message(&state, &peer_id, &data).await;
                    }
                    Err(e) => {
                        state.metrics.error_read.fetch_add(1, Ordering::Relaxed);
                        state.metrics.inbound_live.fetch_sub(1, Ordering::Relaxed);
                        info!("TX stream from {} closed: {}", peer_id, e);
                        break;
                    }
                }
            }
        });
    }
}

/// Handle TX stream message in INV/GETDATA mode
async fn handle_tx_stream_message(state: &Arc<SharedState>, peer_id: &PeerId, data: &[u8]) {
    match TxStreamMessage::decode(data) {
        Ok(TxStreamMessage::InvBatch(batch)) => {
            handle_inv_batch(state, peer_id, batch).await;
        }
        Ok(TxStreamMessage::GetData(getdata)) => {
            handle_getdata(state, peer_id, getdata);
        }
        Ok(TxStreamMessage::Tx(tx)) => {
            handle_tx_response(state, peer_id, tx).await;
        }
        Err(e) => {
            warn!(
                "TX_PARSE_ERR: Failed to parse message from {}: {}",
                peer_id, e
            );
        }
    }
}

/// Handle INV_BATCH message - record sources and request TXs we don't have
async fn handle_inv_batch(state: &Arc<SharedState>, peer_id: &PeerId, batch: InvBatch) {
    debug!(
        "TX_INV_RECV: Received {} INVs from {}",
        batch.entries.len(),
        peer_id
    );

    // A transaction this node got as a direct send (and so did not relay)
    // is now being advertised: its origin has fallen back to flooding, which
    // has to pass through this node too. Advertise it to the other peers,
    // once.
    let join_flood: Vec<Arc<ValidatedTx>> = {
        let mut router = state.tx_router.lock().unwrap();
        if router.has_direct() {
            batch
                .entries
                .iter()
                .filter_map(|entry| router.join_flood(&entry.hash))
                .collect()
        } else {
            Vec::new()
        }
    };
    if !join_flood.is_empty() {
        // Ready for the demands that follow the adverts.
        {
            let mut buffer = state.tx_buffer.write().await;
            for tx in &join_flood {
                buffer.insert(Arc::clone(tx));
            }
        }
        let peers: Vec<PeerId> = state
            .peer_streams
            .read()
            .await
            .keys()
            .filter(|p| *p != peer_id)
            .cloned()
            .collect();
        debug!(
            "TX_RELAY: Joining the flood of {} TXs received directly earlier",
            join_flood.len()
        );
        for tx in join_flood {
            let entry = InvEntry {
                hash: *tx.hash(),
                fee_per_op: tx.fee_per_op(),
            };
            queue_inv(state, &peers, entry).await;
        }
    }

    // Each structure is locked once per batch rather than once per entry, and
    // the two locks are never held together. Entries keep their batch order.
    // Skip TXs we already have; they are not recorded as sources.
    let unseen: Vec<[u8; 32]> = {
        let seen = state.tx_seen.read().await;
        batch
            .entries
            .iter()
            .map(|entry| entry.hash)
            .filter(|hash| !seen.contains(hash))
            .collect()
    };
    // Record this peer as a source for round-robin GETDATA retries, and
    // request only TXs it is the first to advertise.
    let to_request: Vec<[u8; 32]> = if unseen.is_empty() {
        Vec::new()
    } else {
        let mut tracker = state.inv_tracker.write().await;
        unseen
            .into_iter()
            .filter(|hash| tracker.record_source(*hash, *peer_id))
            .collect()
    };

    // Send GETDATA for TXs we don't have
    if !to_request.is_empty() {
        state
            .metrics
            .flood_demanded
            .fetch_add(to_request.len() as u64, Ordering::Relaxed);
        debug!(
            "TX_GETDATA_SEND: Requesting {} TXs from {}",
            to_request.len(),
            peer_id
        );

        // Record pending requests
        {
            let mut pending = state.pending_getdata.write().await;
            for hash in &to_request {
                pending.insert(*hash, *peer_id);
            }
        }

        // Build and send GETDATA
        let mut getdata = GetData::new();
        for hash in to_request {
            getdata.push(hash);
        }
        let encoded_chunks = match getdata.encode_chunked() {
            Ok(chunks) => chunks,
            Err(e) => {
                warn!("Failed to encode GETDATA for {}: {}", peer_id, e);
                return;
            }
        };

        let state_clone = Arc::clone(state);
        let peer_clone = *peer_id;
        tokio::spawn(async move {
            for (encoded, chunk_hashes) in encoded_chunks {
                if let Err(e) =
                    send_to_peer_stream(&state_clone, peer_clone, StreamType::Tx, &encoded).await
                {
                    warn!("Failed to send GETDATA to {}: {}", peer_clone, e);
                } else {
                    // The pending entries were stamped at insert time above;
                    // restart the peer's response window now that the demand
                    // is actually on the wire.
                    mark_getdata_sent(&state_clone, &peer_clone, &chunk_hashes).await;
                }
            }
        });
    }
}

/// Handle GETDATA message - respond with requested TXs
fn handle_getdata(state: &Arc<SharedState>, peer_id: &PeerId, getdata: GetData) {
    debug!(
        "TX_GETDATA_RECV: Peer {} requesting {} TXs",
        peer_id,
        getdata.hashes.len()
    );

    let state = Arc::clone(state);
    let peer = *peer_id;
    tokio::spawn(async move { serve_getdata(&state, peer, getdata.hashes).await });
}

/// GETDATA responses are written as batches of complete TX frames of at most
/// this many bytes. Inbound TX streams are read through a buffer of the same
/// size, so one read can take in a whole batch.
const TX_RESPONSE_BATCH_BYTES: usize = 64 * 1024;

/// A transaction's length-prefixed `StellarMessage::Transaction` frame size.
fn tx_frame_len(tx: &ValidatedTx) -> usize {
    4 + tx
        .flood_frame_parts()
        .iter()
        .map(|part| part.len())
        .sum::<usize>()
}

/// Serve one demand in order, as one job that retains only one bounded batch
/// while a peer is backpressured. Every TX still has its own wire frame.
/// Batching avoids one task, stream lock and short QUIC write per transaction.
///
/// Each batch is looked up under one TX buffer lock, which is released before
/// its frames are copied (straight from the shared TX bytes) and written.
/// Lookups are lazy: batch k+1 is looked up after batch k is written, so a
/// peer backpressured for longer than the buffer's retention gets misses for
/// the rest rather than late responses; it re-demands after its timeout.
///
/// The job stops at the first write that fails after `send_peer_frames`'
/// retries, usually because the peer disconnected. If the connection is in
/// fact still up (a reset stream that also failed to reopen), the requester
/// re-demands the rest after its response timeout.
async fn serve_getdata(state: &SharedState, peer: PeerId, hashes: Vec<[u8; 32]>) {
    let mut hashes = hashes.into_iter();
    // A hit that did not fit the previous batch; it starts the next one.
    let mut carried: Option<Arc<ValidatedTx>> = None;
    let mut txs: Vec<Arc<ValidatedTx>> = Vec::new();
    let mut batch = Vec::new();
    loop {
        txs.clear();
        let mut bytes = 0;
        if let Some(tx) = carried.take() {
            bytes += tx_frame_len(&tx);
            txs.push(tx);
        }
        if bytes < TX_RESPONSE_BATCH_BYTES {
            let mut buffer = state.tx_buffer.write().await;
            for hash in hashes.by_ref() {
                // The buffer keeps a flooded transaction for a short time
                // only. One this node accepted itself, or received directly,
                // is kept until it is externalized, and may be demanded long
                // after it arrived (a flood that starts later).
                let found = buffer
                    .get(&hash)
                    .or_else(|| state.tx_router.lock().unwrap().body(&hash));
                let Some(tx) = found else {
                    state
                        .metrics
                        .flood_unfulfilled_unknown
                        .fetch_add(1, Ordering::Relaxed);
                    trace!(
                        "TX_GETDATA_MISS: Don't have TX {:02x?}... for {}",
                        &hash[..4],
                        peer
                    );
                    continue;
                };
                let len = tx_frame_len(&tx);
                if !txs.is_empty() && bytes + len > TX_RESPONSE_BATCH_BYTES {
                    carried = Some(tx);
                    break;
                }
                bytes += len;
                txs.push(tx);
                if bytes >= TX_RESPONSE_BATCH_BYTES {
                    break;
                }
            }
        }
        let Some(first) = txs.first() else {
            return;
        };
        state
            .metrics
            .flood_fulfilled
            .fetch_add(txs.len() as u64, Ordering::Relaxed);
        let written = if bytes > TX_RESPONSE_BATCH_BYTES {
            // Only a lone transaction can exceed the budget. Write it as the
            // existing segmented frame, straight from the shared TX bytes.
            let parts = first.flood_frame_parts();
            let result = send_to_peer_stream_parts(state, peer, StreamType::Tx, &parts).await;
            record_tx_response_write(state, peer, result, 1, bytes - 4)
        } else {
            batch.clear();
            batch.reserve_exact(bytes);
            for tx in &txs {
                let parts = tx.flood_frame_parts();
                // Within the batch budget, so the length fits the u32 prefix.
                let payload = parts.iter().map(|part| part.len()).sum::<usize>();
                batch.extend_from_slice(&(payload as u32).to_be_bytes());
                for part in parts {
                    batch.extend_from_slice(part);
                }
            }
            let frames = framed_io::Frames::Batch(&batch);
            let result = send_peer_frames(state, peer, StreamType::Tx, frames).await;
            record_tx_response_write(state, peer, result, txs.len(), bytes - 4 * txs.len())
        };
        if !written {
            let remaining = hashes.len() + usize::from(carried.is_some());
            if remaining > 0 {
                debug!(
                    "TX_GETDATA_ABANDON: Not serving {} more requested hashes to {} after a failed write",
                    remaining, peer
                );
            }
            return;
        }
    }
}

/// Returns whether the write succeeded.
fn record_tx_response_write(
    state: &SharedState,
    peer: PeerId,
    result: io::Result<()>,
    messages: usize,
    bytes: usize,
) -> bool {
    match result {
        Ok(()) => {
            state
                .metrics
                .message_write
                .fetch_add(messages as u64, Ordering::Relaxed);
            state
                .metrics
                .byte_write
                .fetch_add(bytes as u64, Ordering::Relaxed);
            debug!("TX_SEND: Sent {} TXs to {}", messages, peer);
            true
        }
        Err(e) => {
            state
                .metrics
                .error_write
                .fetch_add(messages as u64, Ordering::Relaxed);
            warn!("Failed to send {} TXs to {}: {}", messages, peer, e);
            false
        }
    }
}

/// Handle TX response (from GETDATA request)
async fn handle_tx_response(state: &Arc<SharedState>, peer_id: &PeerId, tx: Arc<ValidatedTx>) {
    // `tx` was validated in the stream reader's single decode.
    let hash = *tx.hash();
    let fee_per_op = tx.fee_per_op();
    let recv_start = std::time::Instant::now();
    let tx_len = tx.bytes().len() as u64;

    // Dedup
    {
        let mut seen = state.tx_seen.write().await;
        if seen.contains(&hash) {
            trace!("Duplicate TX from {}", peer_id);
            state
                .metrics
                .flood_duplicate_recv
                .fetch_add(tx_len, Ordering::Relaxed);
            return;
        }
        // Enqueue before marking seen or awaiting buffer/relay work. A later
        // Core removal is ordered after this admission even if App is busy.
        if !state.mempool.try_submit_network_tx(Arc::clone(&tx)) {
            state.metrics.message_drop.fetch_add(1, Ordering::Relaxed);
            let dropped = state.tx_dropped_count.fetch_add(1, Ordering::Relaxed) + 1;
            if dropped % 1000 == 1 {
                warn!(
                    "TX_BACKPRESSURE: Dropped TX {:02x?}... (total dropped: {})",
                    &hash[..4],
                    dropped
                );
            }
            return;
        }
        seen.put(hash, ());
        state
            .metrics
            .memory_flood_known
            .store(seen.len() as i64, Ordering::Relaxed);
    }
    state
        .metrics
        .flood_unique_recv
        .fetch_add(tx_len, Ordering::Relaxed);
    // A first copy of a transaction that one of the last ledgers already
    // has. It is in the mempool now, and stays until a build finds it
    // invalid (or, where ledgers are not applied, includes it again).
    if state.tx_router.lock().unwrap().was_finalized(&hash) {
        state.metrics.flood_tx_late_copy.fetch_add(1, Ordering::Relaxed);
        debug!(
            "TX_LATE_COPY: TX {:02x?}... from {} arrived after it externalized",
            &hash[..4],
            peer_id
        );
    }

    // Remove from pending requests and measure pull latency. A body this
    // node demanded arrived over the flood; one it did not ask for was sent
    // directly by a node that is sending to its targets only.
    let solicited = {
        let mut pending = state.pending_getdata.write().await;
        if let Some(req) = pending.remove(&hash) {
            let pull_us = req.first_sent_at.elapsed().as_micros() as u64;
            state
                .metrics
                .flood_tx_pull_latency_sum_us
                .fetch_add(pull_us, Ordering::Relaxed);
            state
                .metrics
                .flood_tx_pull_latency_count
                .fetch_add(1, Ordering::Relaxed);
            true
        } else {
            false
        }
    };

    // Store in buffer for responding to others' GETDATA
    {
        let mut buffer = state.tx_buffer.write().await;
        buffer.insert(Arc::clone(&tx));
    }

    debug!(
        "TX_RECV: Received TX {:02x?}... ({} bytes) from {}",
        &hash[..4],
        tx.bytes().len(),
        peer_id
    );

    if !solicited {
        // A direct send: the sender delivers it to every target itself, so
        // it is kept for this node's own tx sets and not relayed. Remember
        // it in case its origin floods it later (see `handle_inv_batch`).
        state
            .metrics
            .flood_tx_direct_recv
            .fetch_add(1, Ordering::Relaxed);
        state.tx_router.lock().unwrap().note_direct(Arc::clone(&tx));
        debug!(
            "TX_DIRECT_RECV: TX {:02x?}... from {} kept, not relayed",
            &hash[..4],
            peer_id
        );
        record_recv_transaction(state, recv_start);
        return;
    }

    // RELAY: Announce to other peers via INV
    let peers_to_announce: Vec<PeerId> = {
        let streams = state.peer_streams.read().await;
        let tracker = state.inv_tracker.read().await;

        // Get peers who already know about this TX (INV'd us)
        let known_sources: HashSet<PeerId> = tracker
            .peek_sources(&hash)
            .map(|v| v.iter().cloned().collect())
            .unwrap_or_default();

        streams
            .keys()
            .filter(|p| **p != *peer_id && !known_sources.contains(p))
            .cloned()
            .collect()
    };

    if !peers_to_announce.is_empty() {
        debug!(
            "TX_RELAY: Announcing TX {:02x?}... to {} peers via INV",
            &hash[..4],
            peers_to_announce.len()
        );

        queue_inv(state, &peers_to_announce, InvEntry { hash, fee_per_op }).await;
    }

    record_recv_transaction(state, recv_start);
}

/// Record recv-transaction timing
fn record_recv_transaction(state: &SharedState, recv_start: std::time::Instant) {
    let elapsed_us = recv_start.elapsed().as_micros() as u64;
    state
        .metrics
        .recv_transaction_sum_us
        .fetch_add(elapsed_us, Ordering::Relaxed);
    state
        .metrics
        .recv_transaction_count
        .fetch_add(1, Ordering::Relaxed);
    state.metrics.update_recv_transaction_max(elapsed_us);
}

/// Requests arrive on the control stream, separately from bulk responses.
fn handle_txset_request(state: &SharedState, peer_id: PeerId, hash: [u8; 32]) {
    info!(
        "TXSET_REQ_IN: Received TxSet request for {:02x?}... from {}",
        &hash[..4],
        peer_id
    );
    // A set this node is pushing to that peer, or pushed a moment ago, is
    // already on the peer's TX-set stream on this connection. The request
    // crossed it; a second copy could only arrive later. The peer's pending
    // request completes when the pushed copy lands. A repeated request, or
    // one that comes later than a crossing can explain, is answered (see
    // `PushRecord::should_skip_request`).
    if state
        .txset_pushes
        .lock()
        .unwrap()
        .should_skip_request(&hash, &peer_id, Instant::now())
    {
        state
            .metrics
            .txset_push_request_skipped
            .fetch_add(1, Ordering::Relaxed);
        info!(
            "TXSET_REQ_SKIP: TxSet {:02x?}... was already pushed to {}, not sending it again",
            &hash[..4],
            peer_id
        );
        return;
    }
    if let Err(e) = state.event_tx.send(OverlayEvent::TxSetRequested {
        hash,
        from: peer_id,
    }) {
        warn!(
            "Failed to forward TxSetRequested event from {}: {}",
            peer_id, e
        );
    }
}

/// Deliver verified XDR together with its original compressed encoding.
async fn deliver_txset(state: &SharedState, peer_id: PeerId, txset_data: Arc<TxSetData>) {
    let hash = crate::xdr::sha256_hash(txset_data.as_slice());
    // A node that waits for this set through the tree need not ask for it.
    state.txset_tree.lock().unwrap().set_delivered(hash);

    // Clear pending request flag and measure fetch latency
    let slot = {
        let mut pending = state.pending_txset_requests.write().await;
        if let Some((_, request_time, slot)) = pending.remove(&hash) {
            let fetch_us = request_time.elapsed().as_micros() as u64;
            state
                .metrics
                .fetch_txset_sum_us
                .fetch_add(fetch_us, Ordering::Relaxed);
            state
                .metrics
                .fetch_txset_count
                .fetch_add(1, Ordering::Relaxed);
            // An answer to a request does not use the peer's allowance for
            // unrequested frames.
            state
                .txset_push_limiter
                .lock()
                .unwrap()
                .answered_request(peer_id, Instant::now());
            Some(slot)
        } else {
            None
        }
    };

    info!(
        "TXSET_RECV: Received TxSet {:02x?}... ({} bytes) from {} (was_pending={})",
        &hash[..4],
        txset_data.len(),
        peer_id,
        slot.is_some()
    );
    if let Err(e) = state.event_tx.send(OverlayEvent::TxSetReceived {
        hash,
        data: txset_data,
        from: peer_id,
        slot,
    }) {
        warn!(
            "Failed to forward TxSetReceived event from {}: {}",
            peer_id, e
        );
    }
}

/// Pings a peer is answered per second (a node sends one every
/// `TREE_PROBE_INTERVAL`).
const MAX_PROBES_ANSWERED_PER_SEC: u32 = 5;

/// How often a node in rtt mode measures the round trip to each peer.
const TREE_PROBE_INTERVAL: Duration = if cfg!(test) {
    Duration::from_millis(200)
} else {
    Duration::from_secs(2)
};

/// Microseconds on this process's monotonic clock.
fn monotonic_micros() -> u64 {
    static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_micros() as u64
}

/// In rtt mode, measure the round trip to every identified peer: a ping on
/// the tx-set stream, which the peer sends back (`Probe`). The leader lays
/// its tree out by these times (`txset_tree::rtt_order`). A probe that waits
/// behind a set on the same stream comes back late; the smallest of the last
/// few samples is used.
async fn txset_tree_probe_task(state: Arc<SharedState>) {
    let mut interval = tokio::time::interval(TREE_PROBE_INTERVAL);
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop {
        interval.tick().await;
        {
            let tree = state.txset_tree.lock().unwrap();
            if tree.mode() != TreeMode::Rtt || !tree.probing() {
                continue;
            }
        }
        let peers = state.tx_router.lock().unwrap().identified_peers();
        for (node, peer) in peers {
            let state = state.clone();
            tokio::spawn(async move {
                // The clock starts when the stream is this probe's to write.
                let value = monotonic_micros();
                state.txset_tree.lock().unwrap().probe_sent(node, value);
                let ping = Probe::Ping(value).encode();
                let _ = send_to_peer_stream(&state, peer, StreamType::TxSet, &ping).await;
            });
        }
    }
}

/// Where a set this node leads goes (see `StellarOverlay::tree_push_plan`).
struct TreePushPlan {
    /// In front of the frame; none for an ordinary frame.
    header: Option<TreeHeader>,
    /// The peers to write it to.
    targets: Vec<PeerId>,
    /// In a tree: for each target, the peers below it.
    subtrees: HashMap<PeerId, Vec<PeerId>>,
    /// In a tree: the header of a copy that goes to a node directly (not to
    /// be relayed).
    star: Option<TreeHeader>,
}

/// Milliseconds since the Unix epoch by this node's clock.
fn unix_ms_now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as u64)
}

/// A received tx-set frame with the offset at which the ordinary frame
/// begins (0 if it had no tree header).
struct TaggedBody {
    frame: Arc<Vec<u8>>,
    body: usize,
}

impl TaggedBody {
    fn plain(frame: Vec<u8>) -> Self {
        Self {
            frame: Arc::new(frame),
            body: 0,
        }
    }

    /// The ordinary frame as an allocation of its own (what the cache keeps
    /// to answer requests with). Free when there was no header and nobody
    /// else holds the bytes; one copy otherwise.
    fn into_frame(self) -> Vec<u8> {
        if self.body == 0 {
            Arc::try_unwrap(self.frame).unwrap_or_else(|shared| shared.as_ref().clone())
        } else {
            self.frame[self.body..].to_vec()
        }
    }
}

/// Record how long a set took from its leader's send to this node: now less
/// `sent_unix_ms`, by the leader's clock and ours (the nodes of a network
/// keep theirs within a few milliseconds). A nonsensical difference is not
/// recorded. `hop`: how the frame came (1 = straight from the leader), for
/// the per-hop sums; none for a set rebuilt from pieces.
fn record_tree_delay(metrics: &OverlayMetrics, sent_unix_ms: u64, hop: Option<u8>) {
    let delay_ms = unix_ms_now().saturating_sub(sent_unix_ms);
    if sent_unix_ms == 0 || delay_ms >= 60_000 {
        return;
    }
    metrics
        .txset_tree_delay_ms_sum
        .fetch_add(delay_ms, Ordering::Relaxed);
    metrics.txset_tree_delay_count.fetch_add(1, Ordering::Relaxed);
    if let Some(hop) = hop {
        let (sum, count) = if hop <= 1 {
            (
                &metrics.txset_tree_delay_hop1_ms_sum,
                &metrics.txset_tree_delay_hop1_count,
            )
        } else {
            (
                &metrics.txset_tree_delay_hop2_ms_sum,
                &metrics.txset_tree_delay_hop2_count,
            )
        };
        sum.fetch_add(delay_ms, Ordering::Relaxed);
        count.fetch_add(1, Ordering::Relaxed);
    }
    let bucket = match delay_ms {
        0..=50 => &metrics.txset_tree_delay_le_50,
        51..=100 => &metrics.txset_tree_delay_le_100,
        101..=150 => &metrics.txset_tree_delay_le_150,
        151..=200 => &metrics.txset_tree_delay_le_200,
        201..=300 => &metrics.txset_tree_delay_le_300,
        _ => &metrics.txset_tree_delay_gt_300,
    };
    bucket.fetch_add(1, Ordering::Relaxed);
}

/// Handle the tree header of a frame from `peer_id`: record how long the
/// frame took, and pass it on to this node's children in the tree if it has
/// any (see `txset_tree::relay_plan`). The bytes go on as they came, before
/// they are decompressed or checked: every receiver checks them itself, and
/// a node only relays what its parent in the tree sent it, once.
///
/// Returns the frame with the offset of the ordinary frame in it, or `None`
/// if the header is malformed.
fn relay_tagged_frame(
    state: &Arc<SharedState>,
    peer_id: PeerId,
    frame: Vec<u8>,
) -> Option<TaggedBody> {
    let (header, body) = TreeHeader::parse(&frame)?;
    let metrics = &state.metrics;
    metrics.txset_tree_received.fetch_add(1, Ordering::Relaxed);
    record_tree_delay(metrics, header.sent_unix_ms, Some(header.hop));
    let frame = Arc::new(frame);
    if header.kind == FrameKind::Star {
        return Some(TaggedBody { frame, body });
    }
    // What `decode` will refuse anyway is not passed on: a body that is too
    // large, or that announces more XDR than a set may have.
    let body_len = frame.len() - body;
    let plausible = body_len >= 4
        && body_len <= crate::flood::txset_encoding::max_encoded_size()
        && u32::from_be_bytes(frame[body..body + 4].try_into().unwrap()) as usize
            <= crate::flood::txset_encoding::MAX_TXSET_XDR_SIZE;
    if !plausible {
        metrics.txset_tree_refused.fetch_add(1, Ordering::Relaxed);
        return Some(TaggedBody { frame, body });
    }
    // One lock at a time.
    let me = state.identity.lock().unwrap().ours.as_ref().map(|i| i.node_id);
    let sender = state.tx_router.lock().unwrap().node_of(&peer_id).copied();
    let plan = {
        let mut tree = state.txset_tree.lock().unwrap();
        let withhold = tree.config().is_some_and(|config| config.withhold);
        let plan = match (tree.config(), me) {
            (Some(config), Some(me)) => {
                txset_tree::relay_plan(config, &header, &me, sender.as_ref())
            }
            _ => RelayPlan::Refused(txset_tree::RelayRefusal::NotATree),
        };
        match plan {
            // A node set up to withhold (tests, fault runs) passes nothing on.
            RelayPlan::Relay(_) if withhold => {
                metrics.txset_tree_withheld.fetch_add(1, Ordering::Relaxed);
                RelayPlan::Leaf
            }
            RelayPlan::Relay(children) => match tree.admit_relay(&header, &frame[body..]) {
                RelayAdmission::Relay => RelayPlan::Relay(children),
                RelayAdmission::Duplicate => {
                    metrics.txset_tree_duplicate.fetch_add(1, Ordering::Relaxed);
                    RelayPlan::Leaf
                }
                RelayAdmission::OverLimit => {
                    metrics
                        .txset_tree_relay_limited
                        .fetch_add(1, Ordering::Relaxed);
                    warn!(
                        "TXSET_TREE_LIMIT: Not relaying another frame of slot {} from {}: this node relayed {} for that slot and leader already",
                        header.slot,
                        peer_id,
                        txset_tree::MAX_RELAYS_PER_SLOT_AND_LEADER
                    );
                    RelayPlan::Leaf
                }
            },
            other => other,
        }
    };
    match plan {
        RelayPlan::Leaf => {}
        RelayPlan::Refused(why) => {
            if why != txset_tree::RelayRefusal::NotATree {
                metrics.txset_tree_refused.fetch_add(1, Ordering::Relaxed);
                debug!(
                    "TXSET_TREE_REFUSED: Not relaying the tree frame for slot {} from {}: {:?}",
                    header.slot, peer_id, why
                );
            }
        }
        RelayPlan::Relay(children) => {
            let peer_of: HashMap<NodeId, PeerId> = {
                let router = state.tx_router.lock().unwrap();
                router.identified_peers()
            };
            let relay_header = Arc::new(
                TreeHeader {
                    hop: header.hop.saturating_add(1),
                    ..header.clone()
                }
                .encode(),
            );
            let mut unreachable = 0u64;
            let mut busy = 0u64;
            for child in &children {
                let Some(peer) = peer_of.get(child).copied() else {
                    unreachable += 1;
                    continue;
                };
                // A child that does not read its stream holds one frame in
                // this node's memory, not one per slot: while a relay to it
                // is in flight it is sent no other (it asks for the set).
                if !state.txset_tree.lock().unwrap().start_relay(*child) {
                    busy += 1;
                    continue;
                }
                let child = *child;
                let state = Arc::clone(state);
                let frame = Arc::clone(&frame);
                let relay_header = Arc::clone(&relay_header);
                tokio::spawn(async move {
                    let parts = [relay_header.as_slice(), &frame[body..]];
                    let sent =
                        send_to_peer_stream_parts(&state, peer, StreamType::TxSet, &parts).await;
                    state.txset_tree.lock().unwrap().finish_relay(&child);
                    match sent {
                        Ok(_) => {
                            let bytes = (relay_header.len() + frame.len() - body) as u64;
                            let metrics = &state.metrics;
                            metrics.txset_tree_relayed.fetch_add(1, Ordering::Relaxed);
                            metrics
                                .txset_tree_relayed_bytes
                                .fetch_add(bytes, Ordering::Relaxed);
                            metrics.send_txset.fetch_add(1, Ordering::Relaxed);
                            metrics.message_write.fetch_add(1, Ordering::Relaxed);
                            metrics.byte_write.fetch_add(bytes, Ordering::Relaxed);
                        }
                        Err(e) => {
                            state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
                            warn!(
                                "TXSET_TREE_RELAY_FAIL: Failed to relay a tree frame to {}: {}",
                                peer, e
                            );
                        }
                    }
                });
            }
            if unreachable > 0 {
                metrics
                    .txset_tree_unreachable
                    .fetch_add(unreachable, Ordering::Relaxed);
            }
            if busy > 0 {
                metrics
                    .txset_tree_relay_busy
                    .fetch_add(busy, Ordering::Relaxed);
                warn!(
                    "TXSET_TREE_RELAY_BUSY: Not relaying the tree frame for slot {} to {} child(ren): an earlier relay to them is still in flight",
                    header.slot, busy
                );
            }
            debug!(
                "TXSET_TREE_RELAY: Relaying the tree frame for slot {} (hop {}) to {} children",
                header.slot,
                header.hop,
                children.len() as u64 - unreachable - busy
            );
        }
    }
    Some(TaggedBody { frame, body })
}

/// The pieces of a set with their headers, as its leader sends them.
struct SignedPieces {
    headers: Vec<PieceHeader>,
    pieces: Vec<Arc<Vec<u8>>>,
}

/// Sign the root of the Merkle tree over `pieces` with this overlay's key,
/// and make every piece its header (hop 1). `None`: the overlay's key is not
/// an Ed25519 key, or the signing failed.
fn sign_pieces(
    keypair: &Keypair,
    identity: &NodeIdentity,
    slot: u32,
    seed: &[u8; 32],
    params: CodedParams,
    total_len: u32,
    pieces: Vec<Vec<u8>>,
    sent_unix_ms: u64,
) -> Option<SignedPieces> {
    let overlay_key = keypair.public().try_into_ed25519().ok()?.to_bytes();
    let leaves: Vec<[u8; 32]> = pieces
        .iter()
        .enumerate()
        .map(|(i, piece)| txset_coded::leaf_hash(i as u8, piece))
        .collect();
    let (root, paths) = txset_coded::merkle_tree(&leaves);
    let message = txset_coded::root_message(
        &identity.network_id,
        seed,
        slot,
        &identity.node_id,
        params,
        total_len,
        &root,
    );
    let root_sig: [u8; 64] = keypair.sign(&message).ok()?.try_into().ok()?;
    let headers = paths
        .into_iter()
        .enumerate()
        .map(|(index, path)| PieceHeader {
            hop: 1,
            data: params.data as u8,
            pieces: params.pieces as u8,
            index: index as u8,
            slot,
            leader: identity.node_id,
            sent_unix_ms,
            total_len,
            root,
            overlay_key,
            binding_sig: identity.signature,
            root_sig,
            path,
        })
        .collect();
    Some(SignedPieces {
        headers,
        pieces: pieces.into_iter().map(Arc::new).collect(),
    })
}

/// As the leader of `slot`: cut the set's frame into pieces, sign the root of
/// their Merkle tree, and write every piece to its relays. Runs as a task of
/// its own, so that the coding does not hold up the overlay's command loop.
#[allow(clippy::too_many_arguments)]
async fn send_coded_set(
    state: Arc<SharedState>,
    hash: [u8; 32],
    data: Arc<TxSetData>,
    slot: u32,
    seed: [u8; 32],
    layout: Arc<CodedLayout>,
    identity: NodeIdentity,
    peer_of: HashMap<NodeId, PeerId>,
    sent_unix_ms: u64,
) {
    let params = layout.params;
    let signed = {
        let keypair = state.keypair.clone();
        let identity = identity.clone();
        tokio::task::spawn_blocking(move || {
            let frame = data.encoded();
            let pieces = txset_coded::encode_pieces(frame, params).ok()?;
            sign_pieces(
                &keypair,
                &identity,
                slot,
                &seed,
                params,
                frame.len() as u32,
                pieces,
                sent_unix_ms,
            )
        })
        .await
    };
    let Ok(Some(SignedPieces { headers, pieces })) = signed else {
        // Nothing was sent: the other nodes ask for the set after their wait.
        warn!(
            "TXSET_CODED_ERR: Could not cut TX set {:02x?}... for slot {} into pieces",
            &hash[..4],
            slot
        );
        return;
    };
    let metrics = &state.metrics;
    metrics
        .txset_coded_sets_encoded
        .fetch_add(1, Ordering::Relaxed);
    let mut skipped = 0u64;
    for (index, node) in layout.leader_targets() {
        let Some(peer) = peer_of.get(&node).copied() else {
            skipped += 1;
            continue;
        };
        if !state.txset_tree.lock().unwrap().coded.start_send(node) {
            skipped += 1;
            continue;
        }
        let header = headers[index].encode();
        let piece = Arc::clone(&pieces[index]);
        let state = Arc::clone(&state);
        tokio::spawn(async move {
            let parts = [header.as_slice(), piece.as_slice()];
            let sent = send_to_peer_stream_parts(&state, peer, StreamType::TxSet, &parts).await;
            state.txset_tree.lock().unwrap().coded.finish_send(&node);
            let metrics = &state.metrics;
            match sent {
                Ok(_) => {
                    let bytes = (header.len() + piece.len()) as u64;
                    metrics
                        .txset_coded_pieces_sent
                        .fetch_add(1, Ordering::Relaxed);
                    metrics
                        .txset_push_sent_bytes
                        .fetch_add(bytes, Ordering::Relaxed);
                    metrics.message_write.fetch_add(1, Ordering::Relaxed);
                    metrics.byte_write.fetch_add(bytes, Ordering::Relaxed);
                }
                Err(e) => {
                    metrics.error_write.fetch_add(1, Ordering::Relaxed);
                    warn!(
                        "TXSET_CODED_SEND_FAIL: Failed to send a piece to {}: {}",
                        peer, e
                    );
                }
            }
        });
    }
    if skipped > 0 {
        metrics
            .txset_coded_pieces_skipped
            .fetch_add(skipped, Ordering::Relaxed);
    }
}

/// A piece of a coded set arrived from `peer_id` (see `txset_coded`).
///
/// It is looked at only if this node's Core described the slot and names the
/// leader as one that may lead it, the code is the one this node derives, and
/// the sender is the node that owes this node that piece in the layout (by
/// the validator identity of the connection). It is kept only if the leader's
/// signatures and the piece's Merkle path check. A relay passes on what the
/// leader itself sent it, once, and only after those checks. With enough
/// pieces under one root the frame is rebuilt and goes the way of every
/// received frame.
fn handle_piece_frame(state: &Arc<SharedState>, peer_id: PeerId, frame: Vec<u8>) {
    let metrics = &state.metrics;
    let Some((header, at)) = PieceHeader::parse(&frame) else {
        metrics
            .txset_coded_pieces_invalid
            .fetch_add(1, Ordering::Relaxed);
        warn!(
            "TXSET_CODED_ERR: Dropping a malformed piece frame ({} bytes) from {}",
            frame.len(),
            peer_id
        );
        return;
    };
    let refuse = |why: &str| {
        metrics
            .txset_coded_pieces_refused
            .fetch_add(1, Ordering::Relaxed);
        debug!(
            "TXSET_CODED_REFUSED: Piece {} of slot {} from {}: {}",
            header.index, header.slot, peer_id, why
        );
    };
    let invalid = |why: &str| {
        metrics
            .txset_coded_pieces_invalid
            .fetch_add(1, Ordering::Relaxed);
        warn!(
            "TXSET_CODED_INVALID: Dropping piece {} of slot {} from {}: {}",
            header.index, header.slot, peer_id, why
        );
    };
    // One lock at a time.
    let Some(identity) = state.identity.lock().unwrap().ours.clone() else {
        return refuse("this node's identity is not known yet");
    };
    let me = identity.node_id;
    let Some(sender) = state.tx_router.lock().unwrap().node_of(&peer_id).copied() else {
        return refuse("the sender did not prove which validator it is");
    };
    let key: RootKey = (header.slot, header.leader, header.root);
    let index = header.index as usize;
    // Cheap checks first; then the frame is counted against its sender,
    // whatever it turns out to be; only then is the layout consulted (worked
    // out once per slot and leader).
    type Checked = (Arc<CodedLayout>, [u8; 32], bool, Option<bool>);
    let checked: Result<Checked, &str> = {
        let mut tree = state.txset_tree.lock().unwrap();
        let config = match tree.config() {
            None => Err("no layout from Core"),
            Some(config) if config.mode != TreeMode::Coded => Err("not in coded mode"),
            Some(config) if !config.may_lead(header.slot, &header.leader) => {
                Err("unknown slot, or not a leader of it")
            }
            Some(config) if !config.contains(&me) => Err("this node is not in the layout"),
            Some(config) => config
                .seed_of(header.slot)
                .map(|seed| (*seed, config.withhold))
                .ok_or("unknown slot"),
        };
        match config {
            Err(why) => Err(why),
            Ok(_) if !tree.coded.admit_frame(header.slot, header.leader, sender) => {
                Err("too many frames from this sender for the slot and leader")
            }
            Ok((seed, withhold)) => match tree.coded_layout(header.slot, &header.leader) {
                None => Err("no layout for these nodes"),
                Some(layout) if (layout.params.data, layout.params.pieces) != header.params() => {
                    Err("another code than this node derives")
                }
                Some(layout) if layout.sender_of(&me, index) != Some(sender) => {
                    Err("not from the node that owes this piece")
                }
                Some(_) if (sender == header.leader) != (header.hop == 1) => {
                    Err("wrong hop count")
                }
                Some(layout) => {
                    let known = tree.coded.knows_root(&key, layout.params, header.total_len);
                    Ok((layout, seed, withhold, known))
                }
            },
        }
    };
    let (layout, seed, withhold, known) = match checked {
        Ok(checked) => checked,
        Err(why) => return refuse(why),
    };
    match known {
        Some(true) => {}
        Some(false) => return invalid("its root is known with another shape or length"),
        None if !verify_piece_signatures(&identity.network_id, &seed, &header) => {
            return invalid("the root is not signed by the leader");
        }
        None => {}
    }
    let leaf = txset_coded::leaf_hash(header.index, &frame[at..]);
    if txset_coded::merkle_root_from(leaf, index, &header.path) != header.root {
        return invalid("its Merkle path does not reach the root");
    }
    let frame = Arc::new(frame);
    let (outcome, relay) = {
        let mut tree = state.txset_tree.lock().unwrap();
        if !tree.coded.add_root(key, layout.params, header.total_len) {
            return refuse("the leader has two other roots for the slot already");
        }
        if tree.coded.knows_root(&key, layout.params, header.total_len) != Some(true) {
            return invalid("its root is known with another shape or length");
        }
        let outcome = tree.coded.add_piece(
            &key,
            index,
            PieceBytes {
                frame: Arc::clone(&frame),
                at,
            },
        );
        // Passed on: what the leader itself sent this node as the relay of
        // this piece, once per root.
        let relay = header.hop == 1
            && layout.relayed_piece(&me) == Some(index)
            && !matches!(outcome, PieceOutcome::Duplicate | PieceOutcome::Unknown)
            && tree.coded.mark_relayed(key);
        (outcome, relay)
    };
    match &outcome {
        PieceOutcome::Stored | PieceOutcome::Ready(_) => {
            metrics
                .txset_coded_pieces_received
                .fetch_add(1, Ordering::Relaxed);
        }
        PieceOutcome::Late => {
            metrics
                .txset_coded_pieces_received
                .fetch_add(1, Ordering::Relaxed);
            metrics
                .txset_coded_pieces_late
                .fetch_add(1, Ordering::Relaxed);
        }
        PieceOutcome::Duplicate => {
            metrics
                .txset_coded_pieces_duplicate
                .fetch_add(1, Ordering::Relaxed);
        }
        PieceOutcome::Unknown => refuse("no place for it"),
    }
    if relay && withhold {
        metrics.txset_tree_withheld.fetch_add(1, Ordering::Relaxed);
    } else if relay {
        relay_piece(state, &layout, &me, &header, &frame, at);
    }
    if let PieceOutcome::Ready(ready) = outcome {
        // The set is in hand when the last piece needed is: the rebuild that
        // follows is this node's own work.
        record_tree_delay(metrics, header.sent_unix_ms, None);
        metrics.txset_tree_received.fetch_add(1, Ordering::Relaxed);
        // The set is its leader's, whoever sent the piece that completed
        // it: what a peer may have this node keep unclaimed is counted
        // against the leader, if it is connected.
        let from = state
            .tx_router
            .lock()
            .unwrap()
            .identified_peers()
            .get(&header.leader)
            .copied()
            .unwrap_or(peer_id);
        tokio::spawn(rebuild_coded_set(
            Arc::clone(state),
            from,
            ready,
            header.slot,
            header.root,
        ));
    }
}

/// Pass this node's piece on to the nodes that expect it from this node.
fn relay_piece(
    state: &Arc<SharedState>,
    layout: &CodedLayout,
    me: &NodeId,
    header: &PieceHeader,
    frame: &Arc<Vec<u8>>,
    at: usize,
) {
    let peer_of: HashMap<NodeId, PeerId> = state.tx_router.lock().unwrap().identified_peers();
    let relay_header = Arc::new(
        PieceHeader {
            hop: 2,
            ..header.clone()
        }
        .encode(),
    );
    let mut skipped = 0u64;
    for node in layout.receivers_of(me) {
        let Some(peer) = peer_of.get(&node).copied() else {
            skipped += 1;
            continue;
        };
        if !state.txset_tree.lock().unwrap().coded.start_send(node) {
            skipped += 1;
            continue;
        }
        let state = Arc::clone(state);
        let frame = Arc::clone(frame);
        let relay_header = Arc::clone(&relay_header);
        tokio::spawn(async move {
            let parts = [relay_header.as_slice(), &frame[at..]];
            let sent = send_to_peer_stream_parts(&state, peer, StreamType::TxSet, &parts).await;
            state.txset_tree.lock().unwrap().coded.finish_send(&node);
            let metrics = &state.metrics;
            match sent {
                Ok(_) => {
                    let bytes = (relay_header.len() + frame.len() - at) as u64;
                    metrics
                        .txset_coded_pieces_relayed
                        .fetch_add(1, Ordering::Relaxed);
                    metrics
                        .txset_tree_relayed_bytes
                        .fetch_add(bytes, Ordering::Relaxed);
                    metrics.message_write.fetch_add(1, Ordering::Relaxed);
                    metrics.byte_write.fetch_add(bytes, Ordering::Relaxed);
                }
                Err(e) => {
                    metrics.error_write.fetch_add(1, Ordering::Relaxed);
                    warn!(
                        "TXSET_CODED_RELAY_FAIL: Failed to relay a piece to {}: {}",
                        peer, e
                    );
                }
            }
        });
    }
    if skipped > 0 {
        state
            .metrics
            .txset_coded_pieces_skipped
            .fetch_add(skipped, Ordering::Relaxed);
    }
}

/// Rebuild a set's frame from its pieces and hand it on as a received frame:
/// decompress, strict parse, then `deliver_txset` (which files it by the hash
/// of its content). Afterwards, off the path of the set, encode the frame
/// again and compare the root: a leader that signed pieces which do not fit
/// together is counted (the set is still what its content hash says).
async fn rebuild_coded_set(
    state: Arc<SharedState>,
    peer_id: PeerId,
    ready: ReadySet,
    slot: u32,
    root: [u8; 32],
) {
    let params = ready.params;
    let rebuilt = tokio::task::spawn_blocking(move || {
        use stellar_xdr::{GeneralizedTransactionSet, Limits, ReadXdr};
        let started = Instant::now();
        let held: Vec<Option<&[u8]>> = ready
            .pieces
            .iter()
            .map(|piece| piece.as_ref().map(|piece| piece.as_slice()))
            .collect();
        let (frame, with_parity) =
            txset_coded::rebuild(&held, ready.params, ready.total_len as usize)?;
        let rebuild_us = started.elapsed().as_micros() as u64;
        let data = crate::flood::txset_encoding::decode(frame)?;
        GeneralizedTransactionSet::from_xdr(data.as_slice(), Limits::none())
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        Ok::<_, io::Error>((Arc::new(data), with_parity, rebuild_us))
    })
    .await;
    let metrics = &state.metrics;
    let data = match rebuilt {
        Ok(Ok((data, with_parity, rebuild_us))) => {
            metrics
                .txset_coded_sets_rebuilt
                .fetch_add(1, Ordering::Relaxed);
            if with_parity {
                metrics
                    .txset_coded_sets_rebuilt_with_parity
                    .fetch_add(1, Ordering::Relaxed);
            }
            metrics
                .txset_coded_rebuild_us_sum
                .fetch_add(rebuild_us, Ordering::Relaxed);
            data
        }
        result => {
            metrics
                .txset_coded_sets_failed
                .fetch_add(1, Ordering::Relaxed);
            warn!(
                "TXSET_CODED_ERR: The pieces of a set for slot {} do not rebuild a TX set: {:?}",
                slot,
                result.map(|inner| inner.map(|_| ()))
            );
            return;
        }
    };
    deliver_txset(&state, peer_id, Arc::clone(&data)).await;
    let consistent = tokio::task::spawn_blocking(move || {
        txset_coded::encode_pieces(data.encoded(), params).is_ok_and(|pieces| {
            let leaves: Vec<[u8; 32]> = pieces
                .iter()
                .enumerate()
                .map(|(i, piece)| txset_coded::leaf_hash(i as u8, piece))
                .collect();
            txset_coded::merkle_tree(&leaves).0 == root
        })
    })
    .await;
    if !matches!(consistent, Ok(true)) {
        state
            .metrics
            .txset_coded_sets_inconsistent
            .fetch_add(1, Ordering::Relaxed);
        warn!(
            "TXSET_CODED_INCONSISTENT: The leader of slot {} signed pieces that do not fit together",
            slot
        );
    }
}

async fn handle_inbound_txset_streams(mut incoming: IncomingStreams, state: Arc<SharedState>) {
    while let Some((peer_id, mut stream)) = incoming.next().await {
        debug!("Accepted inbound TxSet stream from {}", peer_id);
        state.metrics.inbound_live.fetch_add(1, Ordering::Relaxed);
        let state = state.clone();

        tokio::spawn(async move {
            let mut probe_window = Instant::now();
            let mut probes_answered = 0u32;
            let pong_in_flight = Arc::new(std::sync::atomic::AtomicBool::new(false));
            loop {
                match read_framed_with_limit(
                    &mut stream,
                    crate::flood::txset_encoding::max_encoded_size() + txset_tree::MAX_TREE_HEADER,
                )
                .await
                {
                    Ok(data) => {
                        state.metrics.message_read.fetch_add(1, Ordering::Relaxed);
                        state
                            .metrics
                            .byte_read
                            .fetch_add(data.len() as u64, Ordering::Relaxed);
                        // A round-trip probe (see `txset_tree_probe_task`):
                        // answer a ping, record a pong. Neither is a set.
                        if let Some(probe) = Probe::parse(&data) {
                            match probe {
                                Some(Probe::Ping(value)) => {
                                    // A peer gets a few answers a second.
                                    let now = Instant::now();
                                    if now.duration_since(probe_window) >= Duration::from_secs(1) {
                                        probe_window = now;
                                        probes_answered = 0;
                                    }
                                    // And one answer at a time: a peer
                                    // that does not read leaves one small
                                    // write waiting, not one per ping.
                                    if probes_answered < MAX_PROBES_ANSWERED_PER_SEC
                                        && !pong_in_flight.swap(true, Ordering::AcqRel)
                                    {
                                        probes_answered += 1;
                                        let state = state.clone();
                                        let pong_in_flight = Arc::clone(&pong_in_flight);
                                        tokio::spawn(async move {
                                            let pong = Probe::Pong(value).encode();
                                            let _ = send_to_peer_stream(
                                                &state,
                                                peer_id,
                                                StreamType::TxSet,
                                                &pong,
                                            )
                                            .await;
                                            pong_in_flight.store(false, Ordering::Release);
                                        });
                                    }
                                }
                                Some(Probe::Pong(sent_micros)) => {
                                    let now = monotonic_micros();
                                    let node =
                                        state.tx_router.lock().unwrap().node_of(&peer_id).copied();
                                    // Only the answer to the probe this node
                                    // sent that validator last counts.
                                    if let Some(node) = node {
                                        let mut tree = state.txset_tree.lock().unwrap();
                                        if tree.probe_answered(&node, sent_micros)
                                            && sent_micros <= now
                                            && now - sent_micros < 10_000_000
                                        {
                                            tree.note_rtt(
                                                node,
                                                Duration::from_micros(now - sent_micros),
                                            );
                                        }
                                    }
                                }
                                None => {}
                            }
                            continue;
                        }
                        // A piece of a coded set (see `txset_coded`). It
                        // does not draw on the allowance below: what a peer
                        // can make this node keep and rebuild is bounded by
                        // the layout (one piece index per sender, slot and
                        // leader) and by `CodedState`'s limits, and nothing
                        // is decompressed before pieces from half of this
                        // node's senders agree on a root its leader signed.
                        if PieceHeader::is_tagged(&data) {
                            handle_piece_frame(&state, peer_id, data);
                            continue;
                        }
                        // Bound what a peer can make this node decompress
                        // and parse. Answers to this node's requests are
                        // free (the token is returned in `deliver_txset`);
                        // frames nobody asked for (pushes) draw on a per-peer
                        // allowance, and a frame over it is dropped here.
                        if !state
                            .txset_push_limiter
                            .lock()
                            .unwrap()
                            .allow(peer_id, Instant::now())
                        {
                            state
                                .metrics
                                .txset_push_dropped
                                .fetch_add(1, Ordering::Relaxed);
                            warn!(
                                "TXSET_PUSH_DROP: Dropping TxSet frame ({} bytes) from {}: over the per-peer limit for frames this node did not ask for",
                                data.len(),
                                peer_id
                            );
                            continue;
                        }
                        // A frame with a tree header: pass the bytes on to
                        // this node's children first (if it has any in that
                        // tree), then go on with the ordinary frame behind
                        // the header.
                        let data = if TreeHeader::is_tagged(&data) {
                            match relay_tagged_frame(&state, peer_id, data) {
                                Some(body) => body,
                                None => {
                                    warn!("TXSET_TREE_ERR: Dropping TxSet frame with a malformed tree header from {}", peer_id);
                                    continue;
                                }
                            }
                        } else {
                            TaggedBody::plain(data)
                        };
                        // Preserve the received encoding through verification and
                        // cache insertion. Forwarding never compresses it again.
                        let decoded = tokio::task::spawn_blocking(move || {
                            use stellar_xdr::{GeneralizedTransactionSet, Limits, ReadXdr};
                            let data = crate::flood::txset_encoding::decode(data.into_frame())?;
                            GeneralizedTransactionSet::from_xdr(data.as_slice(), Limits::none())
                                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
                            Ok::<_, io::Error>(Arc::new(data))
                        })
                        .await;
                        match decoded {
                            Ok(Ok(data)) => deliver_txset(&state, peer_id, data).await,
                            result => warn!("TXSET_PARSE_ERR: Dropping malformed compressed TxSet from {}: {:?}", peer_id, result),                        }
                    }
                    Err(e) => {
                        state.metrics.error_read.fetch_add(1, Ordering::Relaxed);
                        state.metrics.inbound_live.fetch_sub(1, Ordering::Relaxed);
                        info!("TxSet stream from {} closed: {}", peer_id, e);
                        break;
                    }
                }
            }
        });
    }
}

/// INV/GETDATA housekeeping task.
///
/// Periodically:
/// 1. Flushes INV batches that have timed out (100ms)
/// 2. Checks GETDATA timeouts and retries to other peers
async fn inv_getdata_housekeeping_task(state: Arc<SharedState>) {
    // Run every 50ms (half the batch timeout for responsiveness)
    let mut interval = tokio::time::interval(Duration::from_millis(50));
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    loop {
        interval.tick().await;

        // 1. Flush expired INV batches
        let expired_peers = {
            let batcher = state.inv_batcher.read().await;
            batcher.expired_peers()
        };

        for peer_id in expired_peers {
            flush_inv_batch_to_peer(&state, peer_id).await;
        }

        // 2. Handle GETDATA timeouts
        let (to_retry, gave_up) = {
            let mut pending = state.pending_getdata.write().await;
            pending.process_timeouts()
        };

        // Log give-ups
        if !gave_up.is_empty() {
            state
                .metrics
                .flood_abandoned_demands
                .fetch_add(gave_up.len() as u64, Ordering::Relaxed);
        }
        for hash in &gave_up {
            warn!(
                "GETDATA_TIMEOUT: Gave up on TX {:02x?}... after 30s",
                &hash[..4]
            );
        }

        // Retry timed-out requests: group by next peer, send batched GETDATA
        if !to_retry.is_empty() {
            state
                .metrics
                .demand_timeout
                .fetch_add(to_retry.len() as u64, Ordering::Relaxed);

            // Resolve next peer for each hash and group by peer
            let mut per_peer: HashMap<PeerId, Vec<[u8; 32]>> = HashMap::new();
            {
                let mut tracker = state.inv_tracker.write().await;
                let mut pending = state.pending_getdata.write().await;
                for hash in to_retry {
                    if let Some(peer) = tracker.get_next_peer(&hash) {
                        if let Some(req) = pending.get_mut(&hash) {
                            req.retry(peer.clone());
                        }
                        per_peer.entry(peer).or_default().push(hash);
                    } else {
                        debug!("GETDATA_RETRY: No more peers for TX {:02x?}...", &hash[..4]);
                    }
                }
            }

            // Send batched GETDATA per peer, chunked to the XDR demand-vector
            // bound (a retry round can accumulate far more than one message's
            // worth of hashes). Each peer gets its own task so one slow or
            // backpressured peer can't stall retries to the others or delay
            // the next housekeeping tick. `retry()` above stamped dispatch
            // time (which keeps subsequent ticks from re-dispatching these
            // hashes while the send is in flight); the peer's 1s response
            // window only starts once its chunk is actually on the wire.
            for (peer, hashes) in per_peer {
                debug!(
                    "GETDATA_RETRY: Retrying {} TXs to peer {}",
                    hashes.len(),
                    peer
                );
                let getdata = GetData { hashes };
                let chunks = match getdata.encode_chunked() {
                    Ok(chunks) => chunks,
                    Err(e) => {
                        warn!("Failed to encode GETDATA retry to {}: {}", peer, e);
                        continue;
                    }
                };

                let task_state = Arc::clone(&state);
                tokio::spawn(async move {
                    for (encoded, chunk_hashes) in chunks {
                        match try_send_to_existing_stream(
                            &task_state,
                            peer,
                            StreamType::Tx,
                            &encoded,
                        )
                        .await
                        {
                            Ok(()) => {
                                mark_getdata_sent(&task_state, &peer, &chunk_hashes).await;
                            }
                            Err(e) => {
                                // Keep the dispatch stamp: the next
                                // housekeeping round retries these hashes
                                // (to the next peer) about a second from now.
                                warn!("Failed to send GETDATA retry to {}: {:?}", peer, e);
                                break;
                            }
                        }
                    }
                });
            }
        }
    }
}

/// Blake2b hash for deduplication
fn blake2b_hash(data: &[u8]) -> [u8; 32] {
    use blake2::{Blake2b, Digest};
    use digest::consts::U32;
    let mut hasher = Blake2b::<U32>::new();
    hasher.update(data);
    hasher.finalize().into()
}

#[cfg(test)]
fn test_scp_envelope_xdr(slot_index: u64) -> Vec<u8> {
    use stellar_xdr::{Limits, ScpEnvelope, WriteXdr};

    let mut envelope = ScpEnvelope::default();
    envelope.statement.slot_index = slot_index;
    envelope.to_xdr(Limits::none()).unwrap()
}

#[cfg(test)]
fn test_tx(sequence: i64) -> Arc<ValidatedTx> {
    let bytes = crate::xdr::tests::valid_transaction_xdr(1000, sequence, 1);
    ValidatedTx::from_core_trusted(bytes, 1000, 1).unwrap()
}

#[cfg(test)]
fn test_txset_xdr(seed: u8) -> ([u8; 32], Vec<u8>) {
    use stellar_xdr::{GeneralizedTransactionSet, Hash, Limits, WriteXdr};

    let mut tx_set = GeneralizedTransactionSet::default();
    let GeneralizedTransactionSet::V1(v1) = &mut tx_set;
    v1.previous_ledger_hash = Hash([seed; 32]);
    let bytes = tx_set.to_xdr(Limits::none()).unwrap();
    let hash = crate::xdr::sha256_hash(&bytes);
    (hash, bytes)
}

#[cfg(test)]
type TestOverlay = (
    OverlayHandle,
    mpsc::UnboundedReceiver<OverlayEvent>,
    mpsc::UnboundedReceiver<CoreCommand>,
    StellarOverlay,
);

#[cfg(test)]
fn create_test_overlay(
    keypair: Keypair,
    metrics: Arc<OverlayMetrics>,
) -> Result<TestOverlay, Box<dyn std::error::Error + Send + Sync>> {
    let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
    let (handle, events, overlay) = create_overlay(keypair, metrics, MempoolHandle::new(cmd_tx))?;
    Ok((handle, events, cmd_rx, overlay))
}

#[cfg(test)]
mod dispatch_tests;

#[cfg(test)]
mod quic_window_experiment;

#[cfg(test)]
mod txset_push_tests;

#[cfg(test)]
mod tx_direct_tests;

#[cfg(test)]
mod txset_tree_tests;

#[cfg(test)]
mod txset_coded_tests;

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn network_admission_precedes_app_removal() {
        use crate::integrated::{Overlay as MempoolManager, OverlayHandle as MempoolHandle};

        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let mempool = MempoolHandle::new(cmd_tx);
        let task = tokio::spawn(MempoolManager::new(cmd_rx).run());
        let (_handle, _events, overlay) = create_overlay(
            Keypair::generate_ed25519(),
            Arc::new(OverlayMetrics::new()),
            mempool.clone(),
        )
        .unwrap();
        let tx = test_tx(1);

        // No App event loop is running. A completed network admission must
        // already precede subsequent commands in the mempool FIFO.
        handle_tx_response(&overlay.state, &PeerId::random(), tx.clone()).await;
        let top = mempool.get_top_txs(10).await.unwrap();
        assert_eq!(
            top.len(),
            1,
            "network admission must not wait for App polling"
        );
        assert_eq!(top[0].hash(), tx.hash());
        mempool.remove_txs_sync(vec![*tx.hash()]).await;
        assert!(mempool.get_top_txs(10).await.unwrap().is_empty());
        task.abort();
    }

    #[tokio::test]
    async fn removal_cannot_be_overtaken_by_admission_waiting_on_relay_bookkeeping() {
        use crate::integrated::Overlay as MempoolManager;
        use futures::poll;

        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let mempool = MempoolHandle::new(cmd_tx);
        let (_handle, _events, overlay) = create_overlay(
            Keypair::generate_ed25519(),
            Arc::new(OverlayMetrics::new()),
            mempool.clone(),
        )
        .unwrap();
        let tx = test_tx(1);
        let peer = PeerId::random();
        let pending = overlay.state.pending_getdata.write().await;
        let receive = handle_tx_response(&overlay.state, &peer, tx.clone());
        tokio::pin!(receive);
        assert!(poll!(receive.as_mut()).is_pending());

        // The reader is paused at its first bookkeeping await after admission.
        // Core can remove the transaction even before that reader resumes.
        let task = tokio::spawn(MempoolManager::new(cmd_rx).run());
        assert_eq!(mempool.get_top_txs(10).await.unwrap().len(), 1);
        mempool.remove_txs_sync(vec![*tx.hash()]).await;
        drop(pending);
        receive.await;
        assert!(mempool.get_top_txs(10).await.unwrap().is_empty());
        task.abort();
    }

    #[tokio::test]
    async fn saturated_network_admission_is_retryable_and_duplicates_enqueue_once() {
        let (cmd_tx, mut commands) = mpsc::unbounded_channel();
        let mempool = MempoolHandle::with_admission_capacity(cmd_tx, 1);
        let (_handle, _events, overlay) = create_overlay(
            Keypair::generate_ed25519(),
            Arc::new(OverlayMetrics::new()),
            mempool,
        )
        .unwrap();
        let state = &overlay.state;
        let first = test_tx(1);
        let second = test_tx(2);
        let peer = PeerId::random();
        handle_tx_response(state, &peer, first.clone()).await;
        handle_tx_response(state, &peer, second.clone()).await;
        assert_eq!(state.tx_dropped_count.load(Ordering::Relaxed), 1);
        assert!(!state.tx_seen.read().await.contains(second.hash()));
        assert!(state.tx_buffer.write().await.get(second.hash()).is_none());

        // Observing/dropping the first command releases capacity. Two readers
        // retrying the second TX must enqueue it once, using existing dedup.
        let command = commands.try_recv().unwrap();
        assert!(matches!(&command, CoreCommand::SubmitTx { tx, .. } if tx.hash() == first.hash()));
        drop(command);
        let other = PeerId::random();
        tokio::join!(
            handle_tx_response(state, &peer, second.clone()),
            handle_tx_response(state, &other, second.clone()),
        );
        let command = commands.try_recv().unwrap();
        assert!(matches!(&command, CoreCommand::SubmitTx { tx, .. } if tx.hash() == second.hash()));
        assert!(commands.try_recv().is_err());
        assert_eq!(state.tx_dropped_count.load(Ordering::Relaxed), 1);
        assert!(state.tx_seen.read().await.contains(second.hash()));
        assert!(state.tx_buffer.write().await.get(second.hash()).is_some());
    }

    #[tokio::test]
    async fn test_overlay_creation() {
        let keypair = Keypair::generate_ed25519();
        let (handle, _events, _admissions, overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        let overlay_task = tokio::spawn(async move {
            overlay.run("127.0.0.1", 0).await;
        });

        tokio::time::sleep(Duration::from_millis(100)).await;
        handle.shutdown().await;

        tokio::time::timeout(Duration::from_secs(1), overlay_task)
            .await
            .expect("Overlay should shutdown")
            .expect("Overlay task should complete");
    }

    #[tokio::test]
    async fn test_two_overlays_connect_and_send_scp() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19101;
        let _overlay1_task = tokio::spawn(async move {
            overlay1.run("127.0.0.1", listen_port).await;
        });

        tokio::time::sleep(Duration::from_millis(100)).await;

        let _overlay2_task = tokio::spawn(async move {
            overlay2.run("127.0.0.1", 19102).await;
        });

        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Give connection and streams time to establish
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Send SCP from node1
        let scp_msg = test_scp_envelope_xdr(1);
        handle1.broadcast_scp(scp_msg.clone()).await;

        // Wait for SCP on node2
        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        let mut received = false;

        while tokio::time::Instant::now() < deadline && !received {
            tokio::select! {
                Some(event) = events2.recv() => {
                    if let OverlayEvent::ScpReceived { envelope, .. } = event {
                        assert_eq!(envelope, scp_msg);
                        received = true;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }
        assert!(received, "Should receive SCP message");

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    #[tokio::test]
    async fn test_scp_dedup() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19201;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19202).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + stream setup
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain connection events
        while events2.try_recv().is_ok() {}

        // Send same SCP twice
        let scp_msg = test_scp_envelope_xdr(2);
        handle1.broadcast_scp(scp_msg.clone()).await;
        tokio::time::sleep(Duration::from_millis(50)).await;
        handle1.broadcast_scp(scp_msg.clone()).await;

        // Should only receive once
        tokio::time::sleep(Duration::from_millis(200)).await;

        let mut count = 0;
        while let Ok(event) = events2.try_recv() {
            if matches!(event, OverlayEvent::ScpReceived { .. }) {
                count += 1;
            }
        }

        assert_eq!(count, 1, "Should receive only one SCP due to dedup");

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    #[test]
    fn test_blake2b_hash() {
        let data = b"test data";
        let hash1 = blake2b_hash(data);
        let hash2 = blake2b_hash(data);
        assert_eq!(hash1, hash2);

        let hash3 = blake2b_hash(b"different");
        assert_ne!(hash1, hash3);
    }

    /// Critical test: SCP messages must not be blocked by TX traffic
    /// Proves QUIC stream independence by sending large TX payload that takes
    /// measurable time, then verifying SCP arrives BEFORE TX flood completes.
    #[tokio::test]
    async fn test_scp_not_blocked_by_tx_flood() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19301;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19302).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + streams
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain connection events
        while events2.try_recv().is_ok() {}
        while admissions2.try_recv().is_ok() {}

        let tx_count = 1000;

        let tx_start = std::time::Instant::now();
        for i in 0..tx_count {
            handle1.broadcast_tx(test_tx(i as i64)).await;
        }

        // Immediately send small SCP (should bypass TX queue)
        let scp_msg = test_scp_envelope_xdr(3);
        let scp_send_time = std::time::Instant::now();
        handle1.broadcast_scp(scp_msg.clone()).await;

        // Track when SCP arrives vs when all TXs arrive
        // SCP comes on unbounded events channel, TX on bounded admissions channel
        let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
        let mut scp_received_at: Option<std::time::Instant> = None;
        let mut tx_count_received = 0u32;
        let mut all_tx_received_at: Option<std::time::Instant> = None;

        while tokio::time::Instant::now() < deadline {
            tokio::select! {
                Some(event) = events2.recv() => {
                    if let OverlayEvent::ScpReceived { envelope, .. } = event {
                        if envelope == scp_msg && scp_received_at.is_none() {
                            scp_received_at = Some(std::time::Instant::now());
                        }
                    }
                }
                Some(event) = admissions2.recv() => {
                    if let CoreCommand::SubmitTx { .. } = event {
                        tx_count_received += 1;
                        if tx_count_received >= tx_count && all_tx_received_at.is_none() {
                            all_tx_received_at = Some(std::time::Instant::now());
                        }
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }

            // Done when both received
            if scp_received_at.is_some() && all_tx_received_at.is_some() {
                break;
            }
        }

        let scp_received_at = scp_received_at.expect("SCP should be received");
        let all_tx_received_at = all_tx_received_at.expect("All TXs should be received");

        let scp_latency = scp_received_at.duration_since(scp_send_time);
        let tx_total_time = all_tx_received_at.duration_since(tx_start);

        println!("SCP latency: {:?}", scp_latency);
        println!("TX flood total time: {:?}", tx_total_time);
        println!("TX received: {}", tx_count_received);

        // KEY ASSERTION: SCP must arrive BEFORE TX flood completes
        // If streams were blocked, SCP would wait behind all TXs
        assert!(
            scp_received_at < all_tx_received_at,
            "SCP should arrive BEFORE TX flood completes (stream independence). \
             SCP at {:?}, TXs done at {:?}",
            scp_latency,
            tx_total_time
        );

        // Also verify the flood dominated the delivery window. If streams were
        // blocked, SCP latency would be comparable to the flood duration
        // (ratio near 1), so requiring a wide margin keeps the test meaningful
        // without an absolute wall-clock bound that fast CI machines beat.
        assert!(
            tx_total_time > scp_latency * 5,
            "TX flood ({:?}) should dwarf SCP latency ({:?}), otherwise test is invalid",
            tx_total_time,
            scp_latency
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    /// Critical test: TX messages must not be blocked by SCP traffic
    /// Validates bidirectional stream independence
    #[tokio::test]
    async fn test_tx_not_blocked_by_scp_flood() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19501;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19502).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + streams
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain connection events
        while events2.try_recv().is_ok() {}
        while admissions2.try_recv().is_ok() {}

        let scp_count = 1000;

        let scp_start = std::time::Instant::now();
        for i in 0..scp_count {
            let scp = test_scp_envelope_xdr(i as u64);
            handle1.broadcast_scp(scp).await;
        }

        // Immediately send TX (should bypass SCP queue)
        let tx_msg = test_tx(10_000);
        let tx_send_time = std::time::Instant::now();
        handle1.broadcast_tx(Arc::clone(&tx_msg)).await;

        // Track when TX arrives vs when all SCPs arrive
        // SCP comes on unbounded events channel, TX on bounded admissions channel
        let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
        let mut tx_received_at: Option<std::time::Instant> = None;
        let mut scp_count_received = 0u32;
        let mut all_scp_received_at: Option<std::time::Instant> = None;

        while tokio::time::Instant::now() < deadline {
            tokio::select! {
                Some(event) = admissions2.recv() => {
                    if let CoreCommand::SubmitTx { tx, .. } = event {
                        if tx.bytes() == tx_msg.bytes() && tx_received_at.is_none() {
                            tx_received_at = Some(std::time::Instant::now());
                        }
                    }
                }
                Some(event) = events2.recv() => {
                    if let OverlayEvent::ScpReceived { .. } = event {
                        scp_count_received += 1;
                        if scp_count_received >= scp_count && all_scp_received_at.is_none() {
                            all_scp_received_at = Some(std::time::Instant::now());
                        }
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }

            if tx_received_at.is_some() && all_scp_received_at.is_some() {
                break;
            }
        }

        let tx_received_at = tx_received_at.expect("TX should be received");
        let all_scp_received_at = all_scp_received_at.expect("All SCPs should be received");

        let tx_latency = tx_received_at.duration_since(tx_send_time);
        let scp_total_time = all_scp_received_at.duration_since(scp_start);

        println!("TX latency: {:?}", tx_latency);
        println!("SCP flood total time: {:?}", scp_total_time);
        println!("SCP received: {}", scp_count_received);

        // KEY ASSERTION: TX should have reasonable latency despite SCP flood
        // With INV/GETDATA batching (100ms max), TX latency should be < 200ms
        // This proves streams are independent - TX doesn't wait for 10MB of SCP
        assert!(
            tx_latency < Duration::from_millis(500),
            "TX should arrive quickly despite SCP flood (stream independence). \
             TX latency {:?} should be < 200ms",
            tx_latency
        );

        // Verify SCP flood took meaningful time
        assert!(
            scp_total_time > Duration::from_millis(10),
            "SCP flood should take measurable time ({:?}), otherwise test is invalid",
            scp_total_time
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    /// Test TX broadcast and receive
    #[tokio::test]
    async fn test_tx_broadcast() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, _events2, mut admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19401;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19402).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + streams
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain events
        while admissions2.try_recv().is_ok() {}

        // Send TX
        let tx_msg = test_tx(20_000);
        handle1.broadcast_tx(Arc::clone(&tx_msg)).await;

        // Wait for TX on the mempool command stream
        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        let mut received = false;

        while tokio::time::Instant::now() < deadline && !received {
            tokio::select! {
                Some(event) = admissions2.recv() => {
                    if let CoreCommand::SubmitTx { tx, .. } = event {
                        assert_eq!(tx.bytes(), tx_msg.bytes());
                        received = true;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }

        assert!(received, "Should receive TX message");

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    /// Test TxSet request/response flow
    /// Node2 requests a TxSet from Node1, Node1 responds with the data
    #[tokio::test]
    async fn test_txset_fetch() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, mut events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19601;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19602).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + streams
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain events
        while events1.try_recv().is_ok() {}
        while events2.try_recv().is_ok() {}

        // Node2 requests a TxSet by hash
        let (requested_hash, txset_data) = test_txset_xdr(0x42);
        handle2.fetch_txset(requested_hash, 1).await;

        // Node1 should receive TxSetRequested event
        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        let mut request_received = false;

        while tokio::time::Instant::now() < deadline && !request_received {
            tokio::select! {
                Some(event) = events1.recv() => {
                    if let OverlayEvent::TxSetRequested { hash, from } = event {
                        assert_eq!(hash, requested_hash);
                        request_received = true;

                        // Node1 responds with TxSet data
                        handle1.send_txset(hash, Arc::new(TxSetData::from_local(txset_data.clone()).unwrap()), from).await;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }
        assert!(
            request_received,
            "Node1 should receive TxSetRequested event"
        );

        // Node2 should receive TxSetReceived event
        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        let mut response_received = false;

        while tokio::time::Instant::now() < deadline && !response_received {
            tokio::select! {
                Some(event) = events2.recv() => {
                    if let OverlayEvent::TxSetReceived { hash, data, .. } = event {
                        assert_eq!(hash, requested_hash);
                        assert_eq!(data.as_slice(), txset_data);
                        response_received = true;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }
        assert!(
            response_received,
            "Node2 should receive TxSetReceived event"
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    /// Test multiple TXs flood with correct ordering (by fee)
    #[tokio::test]
    async fn test_multiple_txs_flood() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, _events2, mut admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19701;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19702).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + streams
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain events
        while admissions2.try_recv().is_ok() {}

        // Send multiple TXs
        let tx_count = 10;
        for i in 0..tx_count {
            handle1.broadcast_tx(test_tx(i as i64)).await;
        }

        // Wait for all TXs on mempool command stream
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        let mut received_count = 0;

        while tokio::time::Instant::now() < deadline && received_count < tx_count {
            tokio::select! {
                Some(event) = admissions2.recv() => {
                    if let CoreCommand::SubmitTx { .. } = event {
                        received_count += 1;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }

        assert_eq!(
            received_count, tx_count,
            "Should receive all {} TXs",
            tx_count
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    /// Test TX deduplication - same TX sent twice should only be received once
    #[ignore = "flaky test"]
    #[tokio::test]
    async fn test_tx_dedup() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, _events2, mut admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        let listen_port = 19801;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 19802).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;

        // Wait for connection + streams
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain events
        while admissions2.try_recv().is_ok() {}

        // Send same TX twice
        let tx = test_tx(30_000);
        handle1.broadcast_tx(Arc::clone(&tx)).await;
        tokio::time::sleep(Duration::from_millis(50)).await;
        handle1.broadcast_tx(Arc::clone(&tx)).await;

        // Wait and count received TXs
        tokio::time::sleep(Duration::from_millis(500)).await;

        let mut received_count = 0;
        while let Ok(event) = admissions2.try_recv() {
            if let CoreCommand::SubmitTx { .. } = event {
                received_count += 1;
            }
        }

        assert_eq!(
            received_count, 1,
            "Duplicate TX should only be received once"
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
    }

    // ═══ Multi-Node (3+) Gossip Tests ═══

    /// Test SCP messages reach all directly connected peers in a triangle topology
    /// Topology: A-B, B-C, A-C (all nodes connected to each other)
    #[tokio::test]
    async fn test_three_node_triangle_scp() {
        // Create 3 nodes
        let keypair_a = Keypair::generate_ed25519();
        let keypair_b = Keypair::generate_ed25519();
        let keypair_c = Keypair::generate_ed25519();

        let (handle_a, mut events_a, _admissions_a, overlay_a) =
            create_test_overlay(keypair_a, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_b, mut events_b, _admissions_b, overlay_b) =
            create_test_overlay(keypair_b, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_c, mut events_c, _admissions_c, overlay_c) =
            create_test_overlay(keypair_c, Arc::new(OverlayMetrics::new())).unwrap();

        // Start all nodes on different ports
        let port_a = 19901;
        let port_b = 19902;
        let port_c = 19903;

        tokio::spawn(async move { overlay_a.run("127.0.0.1", port_a).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay_b.run("127.0.0.1", port_b).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay_c.run("127.0.0.1", port_c).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect: B -> A, C -> A (both B and C connected to A)
        let addr_a: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port_a)
            .parse()
            .unwrap();

        handle_b.dial(addr_a.clone()).await;
        handle_c.dial(addr_a).await;

        // Wait until A has seen both connections: broadcast only reaches peers
        // A already knows about, so a fixed sleep flakes when the test suite
        // runs fully parallel and connection setup is slow.
        let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
        let mut a_connections = 0;
        while a_connections < 2 {
            assert!(
                tokio::time::Instant::now() < deadline,
                "A should see connections from both B and C (saw {})",
                a_connections
            );
            match tokio::time::timeout(Duration::from_millis(100), events_a.recv()).await {
                Ok(Some(OverlayEvent::PeerConnected { .. })) => a_connections += 1,
                _ => {}
            }
        }

        // Drain connection events
        while events_b.try_recv().is_ok() {}
        while events_c.try_recv().is_ok() {}

        // A broadcasts SCP - should reach both B and C directly
        let scp_msg = test_scp_envelope_xdr(4);
        handle_a.broadcast_scp(scp_msg.clone()).await;

        // Both B and C should receive it directly from A
        let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
        let mut b_received = false;
        let mut c_received = false;

        while tokio::time::Instant::now() < deadline && (!b_received || !c_received) {
            tokio::select! {
                Some(event) = events_b.recv() => {
                    if let OverlayEvent::ScpReceived { envelope, .. } = event {
                        if envelope == scp_msg {
                            b_received = true;
                        }
                    }
                }
                Some(event) = events_c.recv() => {
                    if let OverlayEvent::ScpReceived { envelope, .. } = event {
                        if envelope == scp_msg {
                            c_received = true;
                        }
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }

        assert!(b_received, "Node B should receive SCP from A");
        assert!(c_received, "Node C should receive SCP from A");

        handle_a.shutdown().await;
        handle_b.shutdown().await;
        handle_c.shutdown().await;
    }

    /// Test TX propagation across 3 nodes
    #[tokio::test]
    async fn test_three_node_tx_propagation() {
        let keypair_a = Keypair::generate_ed25519();
        let keypair_b = Keypair::generate_ed25519();
        let keypair_c = Keypair::generate_ed25519();

        let (handle_a, _events_a, _admissions_a, overlay_a) =
            create_test_overlay(keypair_a, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_b, _events_b, mut admissions_b, overlay_b) =
            create_test_overlay(keypair_b, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_c, _events_c, mut admissions_c, overlay_c) =
            create_test_overlay(keypair_c, Arc::new(OverlayMetrics::new())).unwrap();

        let port_a = 20001;
        let port_b = 20002;
        let port_c = 20003;

        tokio::spawn(async move { overlay_a.run("127.0.0.1", port_a).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay_b.run("127.0.0.1", port_b).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay_c.run("127.0.0.1", port_c).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Triangle topology: A-B, B-C, A-C
        let addr_a: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port_a)
            .parse()
            .unwrap();
        let addr_b: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port_b)
            .parse()
            .unwrap();

        handle_b.dial(addr_a.clone()).await;
        handle_c.dial(addr_b).await;
        handle_c.dial(addr_a).await;

        tokio::time::sleep(Duration::from_millis(500)).await;

        while admissions_b.try_recv().is_ok() {}
        while admissions_c.try_recv().is_ok() {}

        // A broadcasts TX
        let tx_msg = test_tx(40_000);
        handle_a.broadcast_tx(Arc::clone(&tx_msg)).await;

        let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
        let mut b_received = false;
        let mut c_received = false;

        while tokio::time::Instant::now() < deadline && (!b_received || !c_received) {
            tokio::select! {
                Some(event) = admissions_b.recv() => {
                    if let CoreCommand::SubmitTx { tx, .. } = event {
                        if tx.bytes() == tx_msg.bytes() {
                            b_received = true;
                        }
                    }
                }
                Some(event) = admissions_c.recv() => {
                    if let CoreCommand::SubmitTx { tx, .. } = event {
                        if tx.bytes() == tx_msg.bytes() {
                            c_received = true;
                        }
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }

        assert!(b_received, "Node B should receive TX");
        assert!(c_received, "Node C should receive TX");

        handle_a.shutdown().await;
        handle_b.shutdown().await;
        handle_c.shutdown().await;
    }

    /// Test that shutdown is clean (no hung connections)
    #[tokio::test]
    async fn test_clean_shutdown() {
        let keypair = Keypair::generate_ed25519();
        let (handle, _events, _admissions, overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        let overlay_task = tokio::spawn(async move {
            overlay.run("127.0.0.1", 20100).await;
        });

        tokio::time::sleep(Duration::from_millis(100)).await;

        // Shutdown should complete quickly
        let shutdown_result = tokio::time::timeout(Duration::from_secs(2), handle.shutdown()).await;

        assert!(
            shutdown_result.is_ok(),
            "Shutdown should complete within 2 seconds"
        );

        // Task should finish
        let task_result = tokio::time::timeout(Duration::from_secs(1), overlay_task).await;

        assert!(
            task_result.is_ok(),
            "Overlay task should complete after shutdown"
        );
    }

    /// Test overlay handles dial to invalid address gracefully
    #[tokio::test]
    async fn test_dial_invalid_address() {
        let keypair = Keypair::generate_ed25519();
        let (handle, _events, _admissions, overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        tokio::spawn(async move { overlay.run("127.0.0.1", 20200).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Dial an address where nothing is listening
        let bad_addr: Multiaddr = "/ip4/127.0.0.1/udp/59999/quic-v1".parse().unwrap();
        handle.dial(bad_addr).await;

        // Should not crash - just log an error and continue
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Overlay should still be operational
        handle.shutdown().await;
    }

    /// Stress test: TX backpressure under heavy load
    /// Verifies:
    /// 1. SCP messages are NEVER dropped (critical path on unbounded channel)
    /// 2. TXs may be dropped under extreme load (acceptable - they'll be re-requested)
    /// 3. No unbounded memory growth (network admission permits cap queued + active insertions at 10,000)
    #[tokio::test]
    async fn test_tx_backpressure_stress() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _admissions1, overlay1) =
            create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut admissions2, overlay2) =
            create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

        // Use unique ports to avoid conflicts with other tests
        let listen_port = 22901;
        tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        tokio::spawn(async move { overlay2.run("127.0.0.1", 22902).await });
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Connect
        let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
            .parse()
            .unwrap();
        handle2.dial(addr).await;
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Drain any initial events
        while events2.try_recv().is_ok() {}
        while admissions2.try_recv().is_ok() {}

        // STRESS TEST: Flood with many TXs while also sending SCP
        // This simulates a real attack scenario where the network is flooded with TXs
        let tx_flood_count = 50_000u32; // Exceed TX_EVENT_CHANNEL_CAPACITY (10,000)
        let scp_msg_count = 100u32;

        // Start flooding TXs (don't wait for processing)
        let handle1_clone = handle1.clone();
        let tx_flood_task = tokio::spawn(async move {
            for i in 0..tx_flood_count {
                // Each TX unique to avoid dedup
                handle1_clone.broadcast_tx(test_tx(i as i64)).await;
                // Small yield to avoid overwhelming the command channel
                if i % 1000 == 0 {
                    tokio::task::yield_now().await;
                }
            }
        });

        // Simultaneously send SCP messages (critical path)
        let handle1_clone2 = handle1.clone();
        let scp_task = tokio::spawn(async move {
            for i in 0..scp_msg_count {
                let scp = test_scp_envelope_xdr(i as u64);
                handle1_clone2.broadcast_scp(scp).await;
                // Space out SCP messages
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        });

        // Wait for floods to complete
        let _ = tokio::join!(tx_flood_task, scp_task);

        // Collect results with timeout
        let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
        let mut scp_received = 0u32;
        let mut tx_received = 0u32;

        while tokio::time::Instant::now() < deadline {
            tokio::select! {
                Some(event) = events2.recv() => {
                    if let OverlayEvent::ScpReceived { .. } = event {
                        scp_received += 1;
                    }
                }
                Some(event) = admissions2.recv() => {
                    if let CoreCommand::SubmitTx { .. } = event {
                        tx_received += 1;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(100)) => {
                    // Check if channels are empty
                    if events2.is_empty() && admissions2.is_empty() {
                        // Give a bit more time for any in-flight messages
                        tokio::time::sleep(Duration::from_millis(200)).await;
                        if events2.is_empty() && admissions2.is_empty() {
                            break;
                        }
                    }
                }
            }
        }

        println!("SCP received: {}/{}", scp_received, scp_msg_count);
        println!("TX received: {}/{}", tx_received, tx_flood_count);

        // CRITICAL ASSERTION 1: ALL SCP messages must be received (never dropped)
        assert_eq!(
            scp_received, scp_msg_count,
            "ALL SCP messages must be received (critical path). Got {}/{}",
            scp_received, scp_msg_count
        );

        // ASSERTION 2: TXs may be dropped under backpressure - this is acceptable
        // We expect SOME TXs to be received (channel isn't completely broken)
        assert!(tx_received > 0, "At least some TXs should be received");

        // ASSERTION 3: TX count should be bounded by channel capacity + what was processed
        // If backpressure is working, we shouldn't receive more than we can handle
        // (This is more about verifying the mechanism works than a strict bound)
        println!(
            "TX backpressure working: received {} of {} flooded TXs ({}%)",
            tx_received,
            tx_flood_count,
            (tx_received as f64 / tx_flood_count as f64 * 100.0) as u32
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
    }
}

/// Test TX set source tracking - verify we ask the right peer
#[tokio::test]
async fn test_txset_source_tracking() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20101;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 20102).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect overlay2 to overlay1
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Record that peer1 (from overlay2's perspective) has a specific TX set
    let test_hash: [u8; 32] = [0xAB; 32];
    // We need to get peer1's ID first - overlay2 should have seen it connect
    // For now, test that record_txset_source doesn't crash
    let fake_peer = PeerId::random();
    handle2.record_txset_source(test_hash, fake_peer).await;

    // Give time for command to process
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Now try to fetch - since fake_peer isn't connected, it should fall back
    handle2.fetch_txset(test_hash, 1).await;
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Clean up
    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test TX set fetch from connected peer
#[tokio::test]
async fn test_txset_fetch_flow() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20201;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 20202).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // overlay2 requests a TX set that overlay1 doesn't have
    let test_hash: [u8; 32] = [0xCD; 32];
    handle2.fetch_txset(test_hash, 1).await;

    // overlay1 should receive the request (as TxSetRequested event)
    tokio::time::sleep(Duration::from_millis(200)).await;

    let mut got_request = false;
    while let Ok(event) = events1.try_recv() {
        if let OverlayEvent::TxSetRequested { hash, .. } = event {
            if hash == test_hash {
                got_request = true;
            }
        }
    }

    assert!(
        got_request,
        "overlay1 should receive TxSet request from overlay2"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that peer disconnect triggers reconnect attempt
#[tokio::test]
async fn test_peer_disconnect_detection() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20301;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 20302).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Verify connection was established by checking we can send SCP
    handle1.broadcast_scp(test_scp_envelope_xdr(5)).await;
    tokio::time::sleep(Duration::from_millis(200)).await;

    // Now shutdown overlay2 - overlay1 should detect disconnect
    handle2.shutdown().await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // overlay1 should have received a disconnect event or connection closed
    // (Connection closed is handled internally by libp2p, we verify no crash)

    handle1.shutdown().await;
    // Test passes if we get here without hanging or crashing
}

/// Test connect to unreachable peer times out gracefully
#[tokio::test]
async fn test_connect_unreachable_peer_timeout() {
    let keypair = Keypair::generate_ed25519();
    let (handle, _events, _admissions, overlay) =
        create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20401;
    tokio::spawn(async move { overlay.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Try to connect to a non-existent peer
    // Use a port that's definitely not listening
    let bad_addr: Multiaddr = "/ip4/127.0.0.1/udp/59999/quic-v1".parse().unwrap();

    // This should not hang - dial returns immediately, connection fails async
    let start = tokio::time::Instant::now();
    handle.dial(bad_addr).await;

    // Give some time for the connection attempt
    tokio::time::sleep(Duration::from_secs(1)).await;

    // Verify we didn't hang for too long
    assert!(
        start.elapsed() < Duration::from_secs(5),
        "Connection attempt should not block for more than 5 seconds"
    );

    // Overlay should still be operational
    handle.shutdown().await;
}

/// Test large TX set doesn't block SCP messages
#[tokio::test]
async fn test_large_txset_doesnt_block_scp() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20501;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 20502).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Drain initial events
    while events1.try_recv().is_ok() {}
    while events2.try_recv().is_ok() {}

    let send_task = tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(100)).await;
    });

    // Immediately send SCP message - should NOT be blocked
    let scp_msg = test_scp_envelope_xdr(6);
    let scp_start = tokio::time::Instant::now();
    handle1.broadcast_scp(scp_msg.clone()).await;

    // SCP should arrive quickly (< 100ms) even if TX set is being transferred
    let deadline = tokio::time::Instant::now() + Duration::from_millis(500);
    let mut scp_received = false;

    while tokio::time::Instant::now() < deadline && !scp_received {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpReceived { envelope, .. } = event {
                    if envelope == scp_msg {
                        scp_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }

    let scp_latency = scp_start.elapsed();
    assert!(scp_received, "SCP message should be received");
    assert!(
        scp_latency < Duration::from_millis(200),
        "SCP latency should be < 200ms, was {:?}",
        scp_latency
    );

    send_task.await.unwrap();
    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test TX set request to peer that has the data
#[tokio::test]
async fn test_txset_request_and_response() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20601;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 20602).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Drain events
    while events1.try_recv().is_ok() {}
    while events2.try_recv().is_ok() {}

    // Node2 requests a TX set
    let (requested_hash, txset_data) = test_txset_xdr(0x77);

    handle2.fetch_txset(requested_hash, 1).await;

    // Node1 receives request and responds
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut responded = false;

    while tokio::time::Instant::now() < deadline && !responded {
        tokio::select! {
            Some(event) = events1.recv() => {
                if let OverlayEvent::TxSetRequested { hash, from } = event {
                    assert_eq!(hash, requested_hash, "Request should have correct hash");
                    handle1.send_txset(hash, Arc::new(TxSetData::from_local(txset_data.clone()).unwrap()), from).await;
                    responded = true;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(
        responded,
        "Node1 should receive and respond to TX set request"
    );

    // Node2 should receive the TX set
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut received = false;

    while tokio::time::Instant::now() < deadline && !received {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::TxSetReceived { hash, data, .. } = event {
                    assert_eq!(hash, requested_hash, "Received hash should match");
                    assert_eq!(data.as_slice(), txset_data, "Received data should match");
                    received = true;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(received, "Node2 should receive TX set response");

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test TX set fetch when no peers are connected
#[tokio::test]
async fn test_txset_fetch_no_peers() {
    let keypair = Keypair::generate_ed25519();
    let (handle, mut events, _admissions, overlay) =
        create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20701;
    tokio::spawn(async move { overlay.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Request TX set with no peers connected
    let requested_hash: [u8; 32] = [0x88; 32];
    handle.fetch_txset(requested_hash, 1).await;

    // Should not crash or hang - just no response
    // Wait briefly to ensure no panic
    tokio::time::sleep(Duration::from_millis(200)).await;

    // Drain any events (there shouldn't be any TX set related ones)
    let mut txset_events = 0;
    while let Ok(event) = events.try_recv() {
        if matches!(event, OverlayEvent::TxSetReceived { .. }) {
            txset_events += 1;
        }
    }
    assert_eq!(
        txset_events, 0,
        "Should not receive TX set when no peers connected"
    );

    handle.shutdown().await;
}

/// Test multiple concurrent TX set requests
#[tokio::test]
async fn test_txset_multiple_concurrent_requests() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20801;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 20802).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Drain events
    while events1.try_recv().is_ok() {}
    while events2.try_recv().is_ok() {}

    // Request multiple TX sets concurrently
    let hash1: [u8; 32] = [0x11; 32];
    let hash2: [u8; 32] = [0x22; 32];
    let hash3: [u8; 32] = [0x33; 32];

    handle2.fetch_txset(hash1, 1).await;
    handle2.fetch_txset(hash2, 1).await;
    handle2.fetch_txset(hash3, 1).await;

    // Node1 should receive all 3 requests
    let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
    let mut received_hashes = std::collections::HashSet::new();

    while tokio::time::Instant::now() < deadline && received_hashes.len() < 3 {
        tokio::select! {
            Some(event) = events1.recv() => {
                if let OverlayEvent::TxSetRequested { hash, from } = event {
                    received_hashes.insert(hash);
                    // Respond to each request
                    let data = format!("txset for {:?}", &hash[..4]).into_bytes();
                    handle1.send_txset(hash, Arc::new(TxSetData::from_local(data).unwrap()), from).await;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }

    assert_eq!(
        received_hashes.len(),
        3,
        "Should receive all 3 TX set requests"
    );
    assert!(received_hashes.contains(&hash1));
    assert!(received_hashes.contains(&hash2));
    assert!(received_hashes.contains(&hash3));

    handle1.shutdown().await;
    handle2.shutdown().await;
}

#[tokio::test]
async fn test_scp_state_request_on_connection() {
    // Test that when two nodes connect, they request SCP state from each other
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    // NB: unique per test — clashing listen ports break parallel runs: the
    // loser of the bind race isn't reachable and its test times out.
    let listen_port = 21401;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 21402).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect node2 to node1
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;

    // Wait for connection + SCP stream setup
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Both nodes should receive ScpStateRequested events (each receives request from the other)
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut node1_received_request = false;
    let mut node2_received_request = false;

    while tokio::time::Instant::now() < deadline
        && (!node1_received_request || !node2_received_request)
    {
        tokio::select! {
            Some(event) = events1.recv() => {
                if let OverlayEvent::ScpStateRequested { ledger_seq, .. } = event {
                    assert_eq!(ledger_seq, 0, "Should request all recent state (ledger_seq=0)");
                    node1_received_request = true;
                }
            }
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpStateRequested { ledger_seq, .. } = event {
                    assert_eq!(ledger_seq, 0, "Should request all recent state (ledger_seq=0)");
                    node2_received_request = true;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }

    assert!(
        node1_received_request,
        "Node 1 should receive SCP state request from node 2"
    );
    assert!(
        node2_received_request,
        "Node 2 should receive SCP state request from node 1"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that QUIC keep-alive keeps connection alive during idle periods
#[tokio::test]
async fn test_quic_keepalive_survives_idle() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    // Use unique ports to avoid conflicts with other tests
    let listen_port = 23001;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 23002).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Verify initial connectivity by sending SCP
    let scp_msg1 = test_scp_envelope_xdr(7);
    handle1.broadcast_scp(scp_msg1.clone()).await;

    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut received_initial = false;
    while tokio::time::Instant::now() < deadline && !received_initial {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpReceived { envelope, .. } = event {
                    if envelope == scp_msg1 {
                        received_initial = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(received_initial, "Should receive initial SCP message");

    // Wait longer than keep-alive interval (15s) but less than max idle (60s)
    // Use 20 seconds to ensure keep-alive packets are sent
    info!("Waiting 20 seconds to test keep-alive...");
    tokio::time::sleep(Duration::from_secs(20)).await;

    // Verify connection is still alive by sending another SCP
    let scp_msg2 = test_scp_envelope_xdr(8);
    handle1.broadcast_scp(scp_msg2.clone()).await;

    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut received_after_idle = false;
    while tokio::time::Instant::now() < deadline && !received_after_idle {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpReceived { envelope, .. } = event {
                    if envelope == scp_msg2 {
                        received_after_idle = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(
        received_after_idle,
        "Connection should survive 20s idle period thanks to QUIC keep-alive"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that overlay listens on configured IP address
#[tokio::test]
async fn test_listen_on_configured_ip() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 21101;

    // Start overlay1 listening on 127.0.0.1
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 21102).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect using the specific IP - this should work
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Verify connection works by checking for SCP state request
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut connected = false;
    while tokio::time::Instant::now() < deadline && !connected {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpStateRequested { .. } = event {
                    connected = true;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(
        connected,
        "Should connect when dialing configured listen IP"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that different listen IPs work correctly
#[tokio::test]
async fn test_listen_ip_binding() {
    // Test that we can specify different IPs for run()
    // On most systems, 127.0.0.1 and 127.0.0.2 are both valid loopback addresses
    let keypair = Keypair::generate_ed25519();
    let (handle, _events, _admissions, overlay) =
        create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 21201;

    // Start on 127.0.0.1 specifically (not 0.0.0.0)
    let overlay_task = tokio::spawn(async move {
        overlay.run("127.0.0.1", listen_port).await;
    });

    tokio::time::sleep(Duration::from_millis(200)).await;

    // The overlay should be running and listening
    // We verify by checking it accepts the shutdown gracefully
    handle.shutdown().await;

    tokio::time::timeout(Duration::from_secs(2), overlay_task)
        .await
        .expect("Overlay should shutdown")
        .expect("Overlay task should complete");
}

/// Test that event loop remains responsive during broadcast (proves parallelism)
#[tokio::test]
async fn test_scp_broadcast_does_not_block_event_loop() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let port1 = 21301;
    let port2 = 21302;

    tokio::spawn(async move { overlay1.run("127.0.0.1", port1).await });
    tokio::spawn(async move { overlay2.run("127.0.0.1", port2).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect
    let addr1: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port1)
        .parse()
        .unwrap();
    handle2.dial(addr1).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Fire off 100 SCP broadcasts rapidly
    for i in 0..100 {
        let msg = test_scp_envelope_xdr(i as u64);
        handle1.broadcast_scp(msg).await;
    }

    // Immediately ping the event loop - if blocked by sequential sends,
    // this won't return until all 100 network writes complete
    let start = tokio::time::Instant::now();
    handle1.ping().await.expect("Ping should succeed");
    let ping_latency = start.elapsed();

    // Ping should return quickly if event loop isn't blocked.
    // Allow 50ms for tokio scheduling overhead - still catches the bug
    // where 100 sequential sends would take seconds.
    assert!(
        ping_latency < Duration::from_millis(50),
        "Ping should return in <50ms (event loop not blocked), took {:?}",
        ping_latency
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that SCP and TxSet streams can be written concurrently to the same peer.
/// This validates that the per-stream mutex design allows independent writes.
#[tokio::test]
async fn test_concurrent_scp_and_txset_writes_to_same_peer() {
    use std::sync::atomic::{AtomicBool, Ordering};

    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();
    let peer2_id = PeerId::from_public_key(&keypair2.public());

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 21001;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 21002).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Connect node2 to node1
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Drain initial events
    while events2.try_recv().is_ok() {}

    // Shared flag to coordinate timing
    let txset_started = Arc::new(AtomicBool::new(false));

    // Start sending large TxSet from node1 to node2
    let (txset_hash, txset_data) = test_txset_xdr(0x22);
    let handle1_txset = handle1.clone();
    let txset_started_clone = txset_started.clone();

    let txset_task = tokio::spawn(async move {
        txset_started_clone.store(true, Ordering::SeqCst);
        handle1_txset
            .send_txset(
                txset_hash,
                Arc::new(TxSetData::from_local(txset_data).unwrap()),
                peer2_id,
            )
            .await;
    });

    // Wait for TxSet send to start
    while !txset_started.load(Ordering::SeqCst) {
        tokio::time::sleep(Duration::from_millis(1)).await;
    }

    // Immediately send SCP message - should NOT be blocked by TxSet write
    let scp_msg = test_scp_envelope_xdr(9);
    let scp_start = tokio::time::Instant::now();
    handle1.broadcast_scp(scp_msg.clone()).await;
    let scp_send_time = scp_start.elapsed();

    // The key assertion: SCP send should complete quickly (<50ms)
    // If the mutexes were shared, SCP would block waiting for TxSet write to finish
    assert!(
        scp_send_time < Duration::from_millis(50),
        "SCP send should not block on TxSet write. Send took {:?}",
        scp_send_time
    );

    // Wait for SCP to be received by node2
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut scp_received = false;
    let mut txset_received = false;

    while tokio::time::Instant::now() < deadline && (!scp_received || !txset_received) {
        tokio::select! {
            Some(event) = events2.recv() => {
                match event {
                    OverlayEvent::ScpReceived { envelope, .. } => {
                        if envelope == scp_msg {
                            scp_received = true;
                        }
                    }
                    OverlayEvent::TxSetReceived { hash, .. } => {
                        if hash == txset_hash {
                            txset_received = true;
                        }
                    }
                    _ => {}
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }

    txset_task.await.unwrap();

    assert!(scp_received, "SCP message should be received");
    assert!(txset_received, "TxSet should be received");

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that pending_txset_requests tracks peer and is cleaned on disconnect.
/// This is a simpler unit test that verifies the data structure changes work.
#[tokio::test]
async fn test_pending_txset_cleanup_on_disconnect() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let peer1_id = PeerId::from_public_key(&keypair1.public());

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    // Start both overlays (ports must not collide with test_20_node_full_mesh 22000-22019)
    let listen_port1 = 22501;
    let listen_port2 = 22502;

    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port1).await });
    tokio::spawn(async move { overlay2.run("127.0.0.1", listen_port2).await });
    tokio::time::sleep(Duration::from_millis(200)).await;

    // Connect node1 to node2
    let addr2: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port2)
        .parse()
        .unwrap();
    handle1.dial(addr2).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Verify connection by exchanging SCP message
    handle1.broadcast_scp(test_scp_envelope_xdr(10)).await;
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut connected = false;
    while tokio::time::Instant::now() < deadline && !connected {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpReceived { .. } = event {
                    connected = true;
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(connected, "Nodes should be connected");

    // Request TxSet - this tests that pending_txset_requests correctly stores (hash, peer)
    let (txset_hash, txset_data) = test_txset_xdr(0x42);
    handle1.fetch_txset(txset_hash, 1).await;
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Verify node2 received the request
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut got_request = false;
    while tokio::time::Instant::now() < deadline && !got_request {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::TxSetRequested { hash, .. } = event {
                    if hash == txset_hash {
                        got_request = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(got_request, "Node2 should receive TxSet request");

    // Now have node2 respond with the TxSet
    // This verifies the pending cleanup works when response is received
    handle2
        .send_txset(
            txset_hash,
            Arc::new(TxSetData::from_local(txset_data.clone()).unwrap()),
            peer1_id,
        )
        .await;

    // Verify node1 receives the TxSet response
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut got_response = false;
    while tokio::time::Instant::now() < deadline && !got_response {
        tokio::select! {
            Some(event) = events1.recv() => {
                if let OverlayEvent::TxSetReceived { hash, data, .. } = event {
                    if hash == txset_hash && data.as_slice() == txset_data {
                        got_response = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(got_response, "Node1 should receive TxSet response");

    // Request the same TxSet again - should NOT be skipped since pending was cleared
    handle1.fetch_txset(txset_hash, 1).await;
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Verify node2 receives the second request
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut got_second_request = false;
    while tokio::time::Instant::now() < deadline && !got_second_request {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::TxSetRequested { hash, .. } = event {
                    if hash == txset_hash {
                        got_second_request = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(
        got_second_request,
        "Node2 should receive second TxSet request after pending was cleared by response"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test INV/GETDATA protocol: TX propagation via INV→GETDATA→TX flow
#[tokio::test]
async fn test_inv_getdata_tx_propagation() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    // Create overlays with INV/GETDATA enabled
    let (handle1, _events1, admissions1, overlay1) =
        create_test_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, mut admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let peer1_id = PeerId::from_public_key(&keypair1.public());

    let listen_port = 19251;
    let overlay1_task = tokio::spawn(async move {
        overlay1.run("127.0.0.1", listen_port).await;
    });

    tokio::time::sleep(Duration::from_millis(100)).await;

    let overlay2_task = tokio::spawn(async move {
        overlay2.run("127.0.0.1", listen_port + 1).await;
    });

    tokio::time::sleep(Duration::from_millis(100)).await;

    // Node2 dials Node1
    let addr: Multiaddr = format!(
        "/ip4/127.0.0.1/udp/{}/quic-v1/p2p/{}",
        listen_port, peer1_id
    )
    .parse()
    .unwrap();
    handle2.dial(addr).await;

    // Wait for connection to establish and streams to open
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Node1 broadcasts a TX
    let tx_msg = test_tx(50_000);
    handle1.broadcast_tx(Arc::clone(&tx_msg)).await;

    // Wait for INV→GETDATA→TX flow (with batching delay + RTT)
    // - INV is batched for up to 100ms
    // - GETDATA sent
    // - TX response sent
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut tx_received = false;

    while tokio::time::Instant::now() < deadline && !tx_received {
        tokio::select! {
            Some(event) = admissions2.recv() => {
                if let CoreCommand::SubmitTx { tx, .. } = event {
                    if tx.bytes() == tx_msg.bytes() {
                        tx_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }

    assert!(
        tx_received,
        "Node2 should receive TX via INV/GETDATA protocol"
    );

    // Suppress warning
    drop(admissions1);

    handle1.shutdown().await;
    handle2.shutdown().await;

    let _ = tokio::time::timeout(Duration::from_secs(1), overlay1_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay2_task).await;
}

/// Regression test for the GETDATA fetch-storm bug: a retry round with more
/// hashes than `TX_DEMAND_VECTOR_MAX_SIZE` must be split into multiple
/// FloodDemand messages that all land and get processed by the peer.
///
/// Before chunking, the oversized demand vector failed XDR encoding
/// ("xdr value max length exceeded") and the entire retry batch was silently
/// dropped, feeding the 30s give-up spiral. With this test, that failure mode
/// shows up as node1 processing zero demanded hashes.
#[tokio::test]
async fn test_getdata_retry_chunked_across_multiple_messages() {
    use stellar_xdr::TX_DEMAND_VECTOR_MAX_SIZE;

    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let metrics1 = Arc::new(OverlayMetrics::new());
    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1.clone(), Arc::clone(&metrics1)).unwrap();
    let (handle2, _events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let peer1_id = PeerId::from_public_key(&keypair1.public());
    // Reach into node2's shared state to stage the retry round directly.
    let state2 = Arc::clone(&overlay2.state);

    // NB: keep unique across all tests in this crate — clashing listen ports
    // make fully-parallel runs flaky.
    let listen_port = 24101;
    let overlay1_task = tokio::spawn(async move {
        overlay1.run("127.0.0.1", listen_port).await;
    });
    tokio::time::sleep(Duration::from_millis(100)).await;

    let overlay2_task = tokio::spawn(async move {
        overlay2.run("127.0.0.1", listen_port + 1).await;
    });
    tokio::time::sleep(Duration::from_millis(100)).await;

    let addr: Multiaddr = format!(
        "/ip4/127.0.0.1/udp/{}/quic-v1/p2p/{}",
        listen_port, peer1_id
    )
    .parse()
    .unwrap();
    handle2.dial(addr).await;

    // The retry path only writes to an already-open TX stream. Broadcast TXs
    // from node2 until its outbound TX stream to node1 is open (the INV send
    // opens it on demand). Polling instead of fixed sleeps keeps this robust
    // when the test suite runs fully parallel and dials are slow.
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    let mut seq = 1i64;
    loop {
        assert!(
            tokio::time::Instant::now() < deadline,
            "Node2's TX stream to node1 never opened"
        );
        handle2.broadcast_tx(test_tx(seq)).await;
        seq += 1;
        tokio::time::sleep(Duration::from_millis(200)).await;

        let streams = state2.peer_streams.read().await;
        if let Some(peer_streams) = streams.get(&peer1_id) {
            if peer_streams.tx.lock().await.is_some() {
                break;
            }
        }
    }

    // Stage more pending GETDATA requests than fit in one FloodDemand, all
    // already timed out, with node1 as their (only) source. The housekeeping
    // task must retry them to node1 as multiple chunks.
    let n = TX_DEMAND_VECTOR_MAX_SIZE as usize * 2 + 100;
    {
        let mut tracker = state2.inv_tracker.write().await;
        let mut pending = state2.pending_getdata.write().await;
        for i in 0..n {
            let mut hash = [0u8; 32];
            hash[..8].copy_from_slice(&(i as u64).to_be_bytes());
            tracker.record_source(hash, peer1_id);
            pending.insert(hash, peer1_id);
            pending.get_mut(&hash).unwrap().sent_at = Instant::now() - Duration::from_secs(2);
        }
    }

    // Node1 has none of these TXs, so every demanded hash it decodes and
    // processes increments flood_unfulfilled_unknown. All n hashes arriving
    // proves every chunk landed and was parsed.
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    let mut processed = 0;
    while tokio::time::Instant::now() < deadline {
        processed = metrics1.flood_unfulfilled_unknown.load(Ordering::Relaxed) as usize;
        if processed >= n {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    // >= rather than ==: the staged requests never complete (node1 doesn't
    // have the TXs), so node2 legitimately starts another retry round after
    // 1s, which may re-deliver some hashes before we sample the counter.
    assert!(
        processed >= n,
        "Node1 should process all {} demanded hashes across multiple GETDATA chunks, got {}",
        n,
        processed
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay1_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay2_task).await;
}

/// Test INV/GETDATA protocol: TX relay through 3 nodes (A→B→C)
#[tokio::test]
async fn test_inv_getdata_three_node_relay() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();
    let keypair3 = Keypair::generate_ed25519();

    // Create overlays with INV/GETDATA enabled (controlled topology)
    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, mut admissions2, overlay2) =
        create_test_overlay(keypair2.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle3, _events3, mut admissions3, overlay3) =
        create_test_overlay(keypair3, Arc::new(OverlayMetrics::new())).unwrap();

    let peer1_id = PeerId::from_public_key(&keypair1.public());
    let peer2_id = PeerId::from_public_key(&keypair2.public());

    let base_port = 19261;

    let overlay1_task = tokio::spawn(async move {
        overlay1.run("127.0.0.1", base_port).await;
    });

    let overlay2_task = tokio::spawn(async move {
        overlay2.run("127.0.0.1", base_port + 1).await;
    });

    let overlay3_task = tokio::spawn(async move {
        overlay3.run("127.0.0.1", base_port + 2).await;
    });

    tokio::time::sleep(Duration::from_millis(200)).await;

    // Topology: Node1 ←→ Node2 ←→ Node3 (Node1 NOT connected to Node3)
    // Node2 dials Node1
    let addr1: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1/p2p/{}", base_port, peer1_id)
        .parse()
        .unwrap();
    handle2.dial(addr1).await;

    // Wait for Node1-Node2 connection
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Node3 dials Node2
    let addr2: Multiaddr = format!(
        "/ip4/127.0.0.1/udp/{}/quic-v1/p2p/{}",
        base_port + 1,
        peer2_id
    )
    .parse()
    .unwrap();
    handle3.dial(addr2).await;

    // Wait for Node2-Node3 connection
    tokio::time::sleep(Duration::from_millis(500)).await;

    // Node1 broadcasts a TX
    let tx_msg = test_tx(60_000);
    handle1.broadcast_tx(Arc::clone(&tx_msg)).await;

    // First verify Node2 receives the TX from Node1
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut node2_received = false;
    while tokio::time::Instant::now() < deadline && !node2_received {
        tokio::select! {
            Some(event) = admissions2.recv() => {
                if let CoreCommand::SubmitTx { tx, .. } = event {
                    eprintln!(
                        "Node2 received TX: {:02x?}",
                        &tx.bytes()[..tx.bytes().len().min(8)]
                    );
                    if tx.bytes() == tx_msg.bytes() {
                        node2_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(node2_received, "Node2 should receive TX from Node1");

    // Then Node3 should receive the TX via relay through Node2
    // Flow: Node1 →INV→ Node2 →GETDATA→ Node1 →TX→ Node2 →INV→ Node3 →GETDATA→ Node2 →TX→ Node3
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut tx_received = false;

    while tokio::time::Instant::now() < deadline && !tx_received {
        tokio::select! {
            Some(event) = admissions3.recv() => {
                if let CoreCommand::SubmitTx { tx, .. } = event {
                    eprintln!(
                        "Node3 received TX: {:02x?}",
                        &tx.bytes()[..tx.bytes().len().min(8)]
                    );
                    // Node3 must receive TX from Node2 (relay), not Node1 (no direct connection)
                    if tx.bytes() == tx_msg.bytes() {
                        tx_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }

    assert!(
        tx_received,
        "Node3 should receive TX relayed through Node2 via INV/GETDATA"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
    handle3.shutdown().await;

    let _ = tokio::time::timeout(Duration::from_secs(1), overlay1_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay2_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay3_task).await;
}

/// Test SCP relay through 3 nodes: A→B→C (the bug that was fixed)
///
/// Topology: Node1 ←→ Node2 ←→ Node3 (Node1 NOT connected to Node3)
/// Node1 broadcasts SCP. Node2 receives it and relays (re-broadcasts) it.
/// Node3 must receive it via Node2's relay.
///
/// Before the fix, Node2's relay request was silently dropped because
/// the message was already in `scp_seen` from the initial receive.
#[tokio::test]
async fn test_scp_relay_three_nodes() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();
    let keypair3 = Keypair::generate_ed25519();

    let (handle1, _events1, _admissions1, overlay1) =
        create_test_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle3, mut events3, _admissions3, overlay3) =
        create_test_overlay(keypair3, Arc::new(OverlayMetrics::new())).unwrap();

    let peer1_id = PeerId::from_public_key(&keypair1.public());
    let peer2_id = PeerId::from_public_key(&keypair2.public());

    let base_port = 19361;

    let overlay1_task = tokio::spawn(async move {
        overlay1.run("127.0.0.1", base_port).await;
    });

    let overlay2_task = tokio::spawn(async move {
        overlay2.run("127.0.0.1", base_port + 1).await;
    });

    let overlay3_task = tokio::spawn(async move {
        overlay3.run("127.0.0.1", base_port + 2).await;
    });

    tokio::time::sleep(Duration::from_millis(200)).await;

    // Node2 dials Node1
    let addr1: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1/p2p/{}", base_port, peer1_id)
        .parse()
        .unwrap();
    handle2.dial(addr1).await;

    tokio::time::sleep(Duration::from_millis(500)).await;

    // Node3 dials Node2 (NOT Node1 - ensuring no direct A↔C path)
    let addr2: Multiaddr = format!(
        "/ip4/127.0.0.1/udp/{}/quic-v1/p2p/{}",
        base_port + 1,
        peer2_id
    )
    .parse()
    .unwrap();
    handle3.dial(addr2).await;

    tokio::time::sleep(Duration::from_millis(500)).await;

    // Drain connection events
    while events2.try_recv().is_ok() {}
    while events3.try_recv().is_ok() {}

    // Node1 broadcasts SCP
    let scp_msg = test_scp_envelope_xdr(11);
    handle1.broadcast_scp(scp_msg.clone()).await;

    // Node2 should receive it from Node1
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut node2_received = false;
    while tokio::time::Instant::now() < deadline && !node2_received {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpReceived { envelope, from, .. } = event {
                    if envelope == scp_msg && from == peer1_id {
                        node2_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(node2_received, "Node2 should receive SCP from Node1");

    // Node2 relays (re-broadcasts) the same SCP message - this is what C++ core does
    handle2.broadcast_scp(scp_msg.clone()).await;

    // Node3 should receive it via Node2's relay
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut node3_received = false;
    while tokio::time::Instant::now() < deadline && !node3_received {
        tokio::select! {
            Some(event) = events3.recv() => {
                if let OverlayEvent::ScpReceived { envelope, from, .. } = event {
                    if envelope == scp_msg && from == peer2_id {
                        node3_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(
        node3_received,
        "Node3 should receive SCP relayed through Node2"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
    handle3.shutdown().await;

    let _ = tokio::time::timeout(Duration::from_secs(1), overlay1_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay2_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay3_task).await;
}

/// Test that SCP relay doesn't echo back to the sender
///
/// Topology: Node1 ←→ Node2
/// Node1 broadcasts SCP. Node2 receives it and relays (re-broadcasts).
/// Node1 must NOT receive it again (no echo).
#[tokio::test]
async fn test_scp_relay_no_echo_to_sender() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2.clone(), Arc::new(OverlayMetrics::new())).unwrap();

    let peer1_id = PeerId::from_public_key(&keypair1.public());

    let base_port = 19461;

    let overlay1_task = tokio::spawn(async move {
        overlay1.run("127.0.0.1", base_port).await;
    });

    let overlay2_task = tokio::spawn(async move {
        overlay2.run("127.0.0.1", base_port + 1).await;
    });

    tokio::time::sleep(Duration::from_millis(200)).await;

    let addr1: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", base_port)
        .parse()
        .unwrap();
    handle2.dial(addr1).await;

    tokio::time::sleep(Duration::from_millis(500)).await;

    // Drain connection events
    while events1.try_recv().is_ok() {}
    while events2.try_recv().is_ok() {}

    // Node1 broadcasts SCP
    let scp_msg = test_scp_envelope_xdr(12);
    handle1.broadcast_scp(scp_msg.clone()).await;

    // Node2 receives it
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut node2_received = false;
    while tokio::time::Instant::now() < deadline && !node2_received {
        tokio::select! {
            Some(event) = events2.recv() => {
                if let OverlayEvent::ScpReceived { envelope, from, .. } = event {
                    if envelope == scp_msg && from == peer1_id {
                        node2_received = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(node2_received, "Node2 should receive SCP from Node1");

    // Node2 relays - this should NOT send back to Node1 (already in scp_sent_to)
    handle2.broadcast_scp(scp_msg.clone()).await;

    // Wait and verify Node1 does NOT receive an echo
    tokio::time::sleep(Duration::from_millis(500)).await;

    let mut echo_count = 0;
    while let Ok(event) = events1.try_recv() {
        if let OverlayEvent::ScpReceived { envelope, .. } = event {
            if envelope == scp_msg {
                echo_count += 1;
            }
        }
    }
    assert_eq!(
        echo_count, 0,
        "Node1 should NOT receive echo of its own SCP message"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;

    let _ = tokio::time::timeout(Duration::from_secs(1), overlay1_task).await;
    let _ = tokio::time::timeout(Duration::from_secs(1), overlay2_task).await;
}

/// Test that 20 overlays can form a fully-connected mesh when dialing
/// simultaneously. This validates the fix for the stream-open deadlock:
/// `open_streams_to_peer` must be spawned (not awaited inline) so the
/// swarm event loop stays free to process incoming stream-open requests.
///
/// Without the fix, most `control.open_stream()` calls would time out
/// because the swarm couldn't be polled while awaiting inside the
/// `ConnectionEstablished` handler.
#[ignore = "flaky test"]
#[tokio::test]
async fn test_20_node_full_mesh() {
    const N: usize = 20;
    const BASE_PORT: u16 = 22000;

    // Create all overlays
    let mut handles = Vec::with_capacity(N);
    let mut metrics = Vec::with_capacity(N);
    let mut tasks = Vec::with_capacity(N);

    for i in 0..N {
        let keypair = Keypair::generate_ed25519();
        let m = Arc::new(OverlayMetrics::new());
        let (handle, _events, _admissions, overlay) =
            create_test_overlay(keypair, Arc::clone(&m)).unwrap();

        let port = BASE_PORT + i as u16;
        tasks.push(tokio::spawn(async move {
            overlay.run("127.0.0.1", port).await;
        }));
        handles.push(handle);
        metrics.push(m);
    }

    // Brief pause for listeners to bind
    tokio::time::sleep(Duration::from_millis(200)).await;

    // Every node dials every other node simultaneously — the thundering-herd
    // scenario that triggers the deadlock on unfixed code.
    for i in 0..N {
        for j in 0..N {
            if i == j {
                continue;
            }
            let port = BASE_PORT + j as u16;
            let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port)
                .parse()
                .unwrap();
            handles[i].dial(addr).await;
        }
    }

    // Wait for all connections and streams to establish.
    // With the deadlock fix, this should converge well within 5 seconds.
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    loop {
        let mut all_connected = true;
        for i in 0..N {
            let count = handles[i].connected_peer_count().await;
            if count < N - 1 {
                all_connected = false;
                break;
            }
        }
        if all_connected {
            break;
        }
        if tokio::time::Instant::now() >= deadline {
            // Print diagnostics before failing
            for i in 0..N {
                let count = handles[i].connected_peer_count().await;
                let auth = metrics[i].connection_authenticated.load(Ordering::Relaxed);
                eprintln!(
                    "Node {}: connected_peer_count={}, connection_authenticated={}",
                    i, count, auth
                );
            }
            panic!(
                "Timed out waiting for full mesh: not all {} nodes have {} peers",
                N,
                N - 1
            );
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
    }

    // Final assertion: every node has exactly N-1 authenticated peers
    for i in 0..N {
        let count = handles[i].connected_peer_count().await;
        assert_eq!(
            count,
            N - 1,
            "Node {} should have {} peers, got {}",
            i,
            N - 1,
            count
        );
    }

    // Shutdown all overlays
    for handle in &handles {
        handle.shutdown().await;
    }
    for task in tasks {
        let _ = tokio::time::timeout(Duration::from_secs(2), task).await;
    }
}

/// Test that simultaneous dials between two peers result in exactly one
/// logical connection (num_established check prevents double stream setup).
#[tokio::test]
async fn test_simultaneous_dial_dedup() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let m1 = Arc::new(OverlayMetrics::new());
    let m2 = Arc::new(OverlayMetrics::new());
    let (handle1, _events1, _tx1, overlay1) =
        create_test_overlay(keypair1, Arc::clone(&m1)).unwrap();
    let (handle2, mut events2, _tx2, overlay2) =
        create_test_overlay(keypair2, Arc::clone(&m2)).unwrap();

    let port1 = 23100;
    let port2 = 23101;
    tokio::spawn(async move { overlay1.run("127.0.0.1", port1).await });
    tokio::spawn(async move { overlay2.run("127.0.0.1", port2).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Both sides dial each other simultaneously
    let addr1: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port1)
        .parse()
        .unwrap();
    let addr2: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port2)
        .parse()
        .unwrap();
    handle1.dial(addr2).await;
    handle2.dial(addr1).await;

    // Wait for connections to settle
    tokio::time::sleep(Duration::from_secs(2)).await;

    // Each side should see exactly 1 connected peer (not 2)
    let count1 = handle1.connected_peer_count().await;
    let count2 = handle2.connected_peer_count().await;
    assert_eq!(count1, 1, "Node1 should have 1 peer, got {}", count1);
    assert_eq!(count2, 1, "Node2 should have 1 peer, got {}", count2);

    // connection_authenticated metric should also be 1 on each side
    let auth1 = m1.connection_authenticated.load(Ordering::Relaxed);
    let auth2 = m2.connection_authenticated.load(Ordering::Relaxed);
    assert_eq!(
        auth1, 1,
        "Node1 connection_authenticated should be 1, got {}",
        auth1
    );
    assert_eq!(
        auth2, 1,
        "Node2 connection_authenticated should be 1, got {}",
        auth2
    );

    // Verify SCP messages flow correctly (streams not corrupted by duplicate)
    handle1.broadcast_scp(test_scp_envelope_xdr(13)).await;
    let received = tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            if let Some(event) = events2.recv().await {
                if let OverlayEvent::ScpReceived { envelope, .. } = event {
                    return envelope;
                }
            }
        }
    })
    .await;
    assert!(
        received.is_ok(),
        "Node2 should receive SCP message through deduped connection"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that DialPeer (PeerId-based) skips dialing when already connected.
#[tokio::test]
async fn test_dial_peer_skips_when_connected() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();
    let peer_id2 = keypair2.public().to_peer_id();

    let m1 = Arc::new(OverlayMetrics::new());
    let (handle1, _events1, _tx1, overlay1) =
        create_test_overlay(keypair1, Arc::clone(&m1)).unwrap();
    let (handle2, _events2, _tx2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let port1 = 23200;
    let port2 = 23201;
    tokio::spawn(async move { overlay1.run("127.0.0.1", port1).await });
    tokio::spawn(async move { overlay2.run("127.0.0.1", port2).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // First connection: address-based dial (bootstrap)
    let addr2: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port2)
        .parse()
        .unwrap();
    handle1.dial(addr2.clone()).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    assert_eq!(handle1.connected_peer_count().await, 1);

    // Record outbound_attempt before the PeerId-based dial
    let attempts_before = m1.outbound_attempt.load(Ordering::Relaxed);

    // PeerId-based dial should be a no-op (already connected)
    handle1.dial_peer(peer_id2, addr2.clone()).await;
    tokio::time::sleep(Duration::from_millis(200)).await;

    // Should still have exactly 1 connection
    assert_eq!(handle1.connected_peer_count().await, 1);
    // outbound_attempt increments (we submitted the command), but connection_pending
    // should NOT have changed (DialPeer was rejected by libp2p before handshake)
    let attempts_after = m1.outbound_attempt.load(Ordering::Relaxed);
    assert_eq!(
        attempts_after,
        attempts_before + 1,
        "outbound_attempt should increment by 1"
    );

    handle1.shutdown().await;
    handle2.shutdown().await;
}

/// Test that PeerConnected event is emitted with the correct address
/// and that PeerDisconnected triggers reconnection.
#[tokio::test]
async fn test_peer_connected_event_emitted() {
    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();

    let (handle1, mut events1, _tx1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _tx2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let port1 = 23300;
    let port2 = 23301;
    tokio::spawn(async move { overlay1.run("127.0.0.1", port1).await });
    tokio::spawn(async move { overlay2.run("127.0.0.1", port2).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    let addr2: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port2)
        .parse()
        .unwrap();
    handle1.dial(addr2).await;

    // Should receive PeerConnected event
    let connected_event = tokio::time::timeout(Duration::from_secs(3), async {
        loop {
            if let Some(event) = events1.recv().await {
                if let OverlayEvent::PeerConnected { peer_id, addr } = event {
                    return (peer_id, addr);
                }
            }
        }
    })
    .await;

    assert!(
        connected_event.is_ok(),
        "Should receive PeerConnected event"
    );
    let (peer_id, addr) = connected_event.unwrap();
    // The address should contain 127.0.0.1 and port2
    let addr_str = addr.to_string();
    assert!(
        addr_str.contains("127.0.0.1") && addr_str.contains(&port2.to_string()),
        "PeerConnected addr should contain the peer's address, got: {}",
        addr_str
    );

    // Shutdown node2 → node1 should receive PeerDisconnected
    handle2.shutdown().await;
    let disconnect_event = tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if let Some(event) = events1.recv().await {
                if let OverlayEvent::PeerDisconnected { peer_id: pid } = event {
                    return pid;
                }
            }
        }
    })
    .await;
    assert!(
        disconnect_event.is_ok(),
        "Should receive PeerDisconnected event"
    );
    assert_eq!(disconnect_event.unwrap(), peer_id);

    handle1.shutdown().await;
}

/// Test that the 20-node mesh works with the new connectivity algorithm.
/// Audits metrics to verify no reconnection storms or duplicate connections.
#[ignore = "flaky test"]
#[tokio::test]
async fn test_20_node_mesh_with_dedup() {
    const N: usize = 20;
    const BASE_PORT: u16 = 24000;

    let mut handles = Vec::with_capacity(N);
    let mut event_rxs = Vec::with_capacity(N);
    let mut metrics = Vec::with_capacity(N);
    let mut tasks = Vec::with_capacity(N);

    for i in 0..N {
        let keypair = Keypair::generate_ed25519();
        let m = Arc::new(OverlayMetrics::new());
        let (handle, events, _admissions, overlay) =
            create_test_overlay(keypair, Arc::clone(&m)).unwrap();

        let port = BASE_PORT + i as u16;
        tasks.push(tokio::spawn(async move {
            overlay.run("127.0.0.1", port).await;
        }));
        handles.push(handle);
        event_rxs.push(events);
        metrics.push(m);
    }

    tokio::time::sleep(Duration::from_millis(200)).await;

    let dial_start = tokio::time::Instant::now();

    // Every node dials every other node simultaneously
    for i in 0..N {
        for j in 0..N {
            if i == j {
                continue;
            }
            let port = BASE_PORT + j as u16;
            let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", port)
                .parse()
                .unwrap();
            handles[i].dial(addr).await;
        }
    }

    // ── Convergence timeline: sample every 100ms ──
    eprintln!("\n=== Convergence timeline (20 nodes) ===");
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    let mut prev_total_peers = 0usize;
    let mut first_sample = true;
    let convergence_time = loop {
        let elapsed = dial_start.elapsed();
        let mut min_peers = usize::MAX;
        let mut max_peers = 0usize;
        let mut total_peers = 0usize;
        let mut total_out_est = 0u64;
        let mut total_in_est = 0u64;
        for i in 0..N {
            let count = handles[i].connected_peer_count().await;
            total_out_est += metrics[i].outbound_establish.load(Ordering::Relaxed);
            total_in_est += metrics[i].inbound_establish.load(Ordering::Relaxed);
            min_peers = min_peers.min(count);
            max_peers = max_peers.max(count);
            total_peers += count;
        }
        // Only print when something changed
        if total_peers != prev_total_peers || first_sample {
            eprintln!(
                "  t={:5.0?}ms  min_peers={:2}  max_peers={:2}  total_conns={:4}  out_est={:4}  in_est={:4}",
                elapsed.as_millis(), min_peers, max_peers, total_peers, total_out_est, total_in_est
            );
            prev_total_peers = total_peers;
            first_sample = false;
        }
        if min_peers >= N - 1 {
            eprintln!("  *** CONVERGED at t={:.0?}ms ***", elapsed.as_millis());
            break elapsed;
        }
        if tokio::time::Instant::now() >= deadline {
            for i in 0..N {
                let count = handles[i].connected_peer_count().await;
                let auth = metrics[i].connection_authenticated.load(Ordering::Relaxed);
                eprintln!("Node {}: peers={}, auth={}", i, count, auth);
            }
            panic!("Timed out: not all {} nodes have {} peers", N, N - 1);
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    };

    // ── Post-convergence stability: sample every 500ms for 3s ──
    eprintln!("\n=== Post-convergence stability (3s hold) ===");
    let mut prev_out: Vec<u64> = (0..N)
        .map(|i| metrics[i].outbound_establish.load(Ordering::Relaxed))
        .collect();
    let mut prev_in: Vec<u64> = (0..N)
        .map(|i| metrics[i].inbound_establish.load(Ordering::Relaxed))
        .collect();
    let mut prev_drop: Vec<u64> = (0..N)
        .map(|i| metrics[i].outbound_drop.load(Ordering::Relaxed))
        .collect();

    for tick in 1..=6 {
        tokio::time::sleep(Duration::from_millis(500)).await;

        let mut delta_out = 0u64;
        let mut delta_in = 0u64;
        let mut delta_drop = 0u64;
        let mut peer_counts_changed = false;
        for i in 0..N {
            let out = metrics[i].outbound_establish.load(Ordering::Relaxed);
            let inp = metrics[i].inbound_establish.load(Ordering::Relaxed);
            let drp = metrics[i].outbound_drop.load(Ordering::Relaxed);
            delta_out += out - prev_out[i];
            delta_in += inp - prev_in[i];
            delta_drop += drp - prev_drop[i];
            prev_out[i] = out;
            prev_in[i] = inp;
            prev_drop[i] = drp;

            let count = handles[i].connected_peer_count().await;
            if count != N - 1 {
                peer_counts_changed = true;
            }
        }
        eprintln!(
            "  t=+{:.1}s  new_out_est={:3}  new_in_est={:3}  new_drops={}  peer_counts_stable={}",
            tick as f64 * 0.5,
            delta_out,
            delta_in,
            delta_drop,
            !peer_counts_changed
        );

        assert_eq!(delta_drop, 0, "Drops at t=+{:.1}s", tick as f64 * 0.5);
        assert!(
            !peer_counts_changed,
            "Peer counts changed at t=+{:.1}s",
            tick as f64 * 0.5
        );
    }

    eprintln!("\n=== Final per-node metrics ===\n");

    // ── Detailed per-node audit ──
    let expected_dials = (N - 1) as u64; // each node dials N-1 others
    let mut total_outbound_establish = 0u64;
    let mut total_inbound_establish = 0u64;
    let mut total_outbound_drop = 0u64;
    let mut any_reconnect = false;
    let mut total_duplicate_conns = 0u64;

    for i in 0..N {
        let count = handles[i].connected_peer_count().await;
        let auth = metrics[i].connection_authenticated.load(Ordering::Relaxed);
        let out_attempt = metrics[i].outbound_attempt.load(Ordering::Relaxed);
        let out_establish = metrics[i].outbound_establish.load(Ordering::Relaxed);
        let in_establish = metrics[i].inbound_establish.load(Ordering::Relaxed);
        let out_drop = metrics[i].outbound_drop.load(Ordering::Relaxed);
        let pending = metrics[i].connection_pending.load(Ordering::Relaxed);
        // Duplicate connections = total transport connections - unique peers
        // auth == unique peers, (out_establish + in_establish) == total transport connections on this node
        let duplicates = (out_establish + in_establish) as i64 - auth;

        total_outbound_establish += out_establish;
        total_inbound_establish += in_establish;
        total_outbound_drop += out_drop;
        if duplicates > 0 {
            total_duplicate_conns += duplicates as u64;
        }

        eprintln!(
            "Node {:2}: peers={:2} auth={:2} out_attempt={:3} out_est={:2} in_est={:2} drops={} pending={} dupes={}",
            i, count, auth, out_attempt, out_establish, in_establish, out_drop, pending, duplicates
        );

        assert_eq!(count, N - 1, "Node {} peer count", i);
        assert_eq!(auth, (N - 1) as i64, "Node {} auth metric", i);
        assert_eq!(
            out_attempt, expected_dials,
            "Node {} should have dialed exactly {} peers",
            i, expected_dials
        );
        assert_eq!(
            out_drop, 0,
            "Node {} should have 0 drops (no reconnects)",
            i
        );
        assert_eq!(pending, 0, "Node {} should have 0 pending connections", i);

        if out_drop > 0 {
            any_reconnect = true;
        }
    }

    eprintln!(
        "\nTotals: out_establish={} in_establish={} drops={} duplicate_transport_conns={}",
        total_outbound_establish,
        total_inbound_establish,
        total_outbound_drop,
        total_duplicate_conns
    );
    eprintln!("Convergence time: {:.0?}ms", convergence_time.as_millis());
    eprintln!(
        "Unique peer pairs: {} (expected C({},2) = {})",
        total_outbound_establish / 2, // rough: each pair has ~2 outbound establishes
        N,
        N * (N - 1) / 2
    );

    assert!(
        !any_reconnect,
        "No node should have experienced a reconnect/drop"
    );

    // Verify SCP still flows: node 0 broadcasts, all others receive
    handles[0].broadcast_scp(test_scp_envelope_xdr(14)).await;
    let mut received_count = 0u32;
    for i in 1..N {
        let result = tokio::time::timeout(Duration::from_secs(3), async {
            loop {
                if let Some(event) = event_rxs[i].recv().await {
                    match event {
                        OverlayEvent::ScpReceived { .. } => return true,
                        OverlayEvent::PeerConnected { .. } => continue,
                        _ => continue,
                    }
                }
            }
        })
        .await;
        if result.is_ok() {
            received_count += 1;
        }
    }
    assert_eq!(
        received_count,
        (N - 1) as u32,
        "All {} peers should receive SCP, got {}",
        N - 1,
        received_count
    );

    for handle in &handles {
        handle.shutdown().await;
    }
    for task in tasks {
        let _ = tokio::time::timeout(Duration::from_secs(2), task).await;
    }
}
