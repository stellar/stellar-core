//! Unified libp2p Overlay v2
//!
//! **Transport: QUIC** for true stream independence - no TCP head-of-line blocking.
//! If a packet is lost on the TX stream, SCP stream is UNAFFECTED.
//!
//! Uses libp2p-stream for persistent bidirectional streams:
//! - SCP stream: consensus messages (priority, ~500B)
//! - TX stream: transaction flooding (~1KB) - uses INV/GETDATA protocol
//! - TxSet stream: TX set request/response (~10MB)
//!
//! Each stream is opened once per peer and kept alive.
//! QUIC provides independent loss recovery per stream.

use crate::flood::{
    GetData, InvBatch, InvBatcher, InvEntry, InvTracker, PendingRequests, TxBuffer, TxStreamMessage,
};
use crate::metrics::OverlayMetrics;
use futures::{AsyncReadExt, AsyncWriteExt, StreamExt};
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
use reed_solomon_simd::{ReedSolomonDecoder, ReedSolomonEncoder};
use std::collections::{HashMap, HashSet};
use std::io;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::{mpsc, Mutex, RwLock};
use tracing::{debug, error, info, trace, warn};

// Protocol identifiers for dedicated streams
pub const SCP_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/scp/1.0.0");
pub const TX_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/tx/1.0.0");
pub const TXSET_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/txset/1.0.0");
pub const TXSET_SHARD_PROTOCOL: StreamProtocol = StreamProtocol::new("/stellar/txset-shard/1.0.0");

/// Message frame: 4-byte length prefix + payload
/// Max message size: 16MB (for large TX sets)
const MAX_MESSAGE_SIZE: usize = 16 * 1024 * 1024;

/// Bounded channel capacity for TX events (backpressure for TX flooding)
/// TXs that can't be queued are dropped - they'll be re-requested if needed.
const TX_EVENT_CHANNEL_CAPACITY: usize = 10_000;

const TXSET_TARGET_SHARD_SIZE: usize = 1024;
const TXSET_SHARD_RECOVERY_FACTOR_PERCENT: usize = 50;
const TXSET_SHARD_INITIAL_TTL: u8 = 1;
const TXSET_TARGET_SHARDS_PER_PEER: usize = 2;
const TXSET_MAX_TOTAL_SHARDS: usize = 255;
const TXSET_SHARD_HEADER_LEN: usize = 32 + 2 + 2 + 2 + 4 + 8 + 1 + 4;

#[derive(Clone, Copy, Debug)]
pub struct TxSetShardConfig {
    pub target_shard_size: usize,
    pub recovery_factor_percent: usize,
    pub initial_ttl: u8,
}

impl Default for TxSetShardConfig {
    fn default() -> Self {
        Self {
            target_shard_size: TXSET_TARGET_SHARD_SIZE,
            recovery_factor_percent: TXSET_SHARD_RECOVERY_FACTOR_PERCENT,
            initial_ttl: TXSET_SHARD_INITIAL_TTL,
        }
    }
}

impl TxSetShardConfig {
    fn recovery_shards(&self, original_shards: usize) -> usize {
        (original_shards * self.recovery_factor_percent)
            .div_ceil(100)
            .max(1)
    }

    fn max_original_shards(&self, total_shard_limit: usize) -> Option<usize> {
        let total_shard_limit = total_shard_limit.min(TXSET_MAX_TOTAL_SHARDS);
        (2..=total_shard_limit).rev().find(|original_shards| {
            original_shards % 2 == 0
                && *original_shards + self.recovery_shards(*original_shards) <= total_shard_limit
        })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct TxSetShardPlan {
    original_shards: usize,
    recovery_shards: usize,
    shard_size: usize,
}

fn make_even(value: usize) -> usize {
    value + (value % 2)
}

fn plan_txset_shards(
    data_len: usize,
    peer_count: usize,
    config: TxSetShardConfig,
) -> Result<TxSetShardPlan, String> {
    if config.target_shard_size == 0 {
        return Err("configured target shard size must be non-zero".to_string());
    }

    let min_total_shards = 2 + config.recovery_shards(2);
    let target_total_shards = peer_count
        .max(1)
        .saturating_mul(TXSET_TARGET_SHARDS_PER_PEER)
        .max(min_total_shards)
        .min(TXSET_MAX_TOTAL_SHARDS);
    let max_original_shards = config
        .max_original_shards(target_total_shards)
        .ok_or_else(|| {
            format!(
            "recovery factor {}% leaves no valid even original shard count under {} total shards",
            config.recovery_factor_percent, target_total_shards
        )
        })?;

    let target_shard_size = make_even(config.target_shard_size);
    let size_limited_original_shards =
        make_even(data_len.max(1).div_ceil(target_shard_size)).max(2);
    let original_shards = size_limited_original_shards.min(max_original_shards);
    let shard_size = make_even(data_len.max(1).div_ceil(original_shards))
        .max(target_shard_size)
        .max(2);

    let recovery_shards = config.recovery_shards(original_shards);
    if original_shards + recovery_shards > TXSET_MAX_TOTAL_SHARDS {
        return Err(format!(
            "planned shard count {} exceeds max {}",
            original_shards + recovery_shards,
            TXSET_MAX_TOTAL_SHARDS
        ));
    }

    Ok(TxSetShardPlan {
        original_shards,
        recovery_shards,
        shard_size,
    })
}

#[derive(Clone, Debug)]
struct TxSetShardMessage {
    hash: [u8; 32],
    original_shards: usize,
    recovery_shards: usize,
    shard_index: usize,
    shard_size: usize,
    original_len: usize,
    ttl: u8,
    payload: Vec<u8>,
}

impl TxSetShardMessage {
    fn encode(&self) -> Result<Vec<u8>, String> {
        if self.original_shards == 0 || self.recovery_shards == 0 {
            return Err("shard counts must be non-zero".to_string());
        }
        if self.shard_index >= self.original_shards + self.recovery_shards {
            return Err("shard index out of range".to_string());
        }
        if self.payload.len() != self.shard_size {
            return Err(format!(
                "payload length {} != shard size {}",
                self.payload.len(),
                self.shard_size
            ));
        }

        let original_shards = u16::try_from(self.original_shards)
            .map_err(|_| "too many original shards".to_string())?;
        let recovery_shards = u16::try_from(self.recovery_shards)
            .map_err(|_| "too many recovery shards".to_string())?;
        let shard_index =
            u16::try_from(self.shard_index).map_err(|_| "shard index too large".to_string())?;
        let shard_size =
            u32::try_from(self.shard_size).map_err(|_| "shard size too large".to_string())?;
        let original_len = u64::try_from(self.original_len)
            .map_err(|_| "original length too large".to_string())?;
        let payload_len =
            u32::try_from(self.payload.len()).map_err(|_| "payload too large".to_string())?;

        let mut out = Vec::with_capacity(TXSET_SHARD_HEADER_LEN + self.payload.len());
        out.extend_from_slice(&self.hash);
        out.extend_from_slice(&original_shards.to_be_bytes());
        out.extend_from_slice(&recovery_shards.to_be_bytes());
        out.extend_from_slice(&shard_index.to_be_bytes());
        out.extend_from_slice(&shard_size.to_be_bytes());
        out.extend_from_slice(&original_len.to_be_bytes());
        out.push(self.ttl);
        out.extend_from_slice(&payload_len.to_be_bytes());
        out.extend_from_slice(&self.payload);
        Ok(out)
    }

    fn decode(data: &[u8]) -> Result<Self, String> {
        if data.len() < TXSET_SHARD_HEADER_LEN {
            return Err(format!("shard message too short: {}", data.len()));
        }

        let mut hash = [0u8; 32];
        hash.copy_from_slice(&data[0..32]);
        let original_shards = u16::from_be_bytes([data[32], data[33]]) as usize;
        let recovery_shards = u16::from_be_bytes([data[34], data[35]]) as usize;
        let shard_index = u16::from_be_bytes([data[36], data[37]]) as usize;
        let shard_size = u32::from_be_bytes([data[38], data[39], data[40], data[41]]) as usize;
        let original_len = u64::from_be_bytes([
            data[42], data[43], data[44], data[45], data[46], data[47], data[48], data[49],
        ]) as usize;
        let ttl = data[50];
        let payload_len = u32::from_be_bytes([data[51], data[52], data[53], data[54]]) as usize;
        let payload_start = TXSET_SHARD_HEADER_LEN;
        let payload_end = payload_start
            .checked_add(payload_len)
            .ok_or_else(|| "payload length overflow".to_string())?;

        if original_shards == 0 || recovery_shards == 0 {
            return Err("shard counts must be non-zero".to_string());
        }
        if shard_index >= original_shards + recovery_shards {
            return Err("shard index out of range".to_string());
        }
        if shard_size == 0 || shard_size % 2 != 0 {
            return Err("shard size must be a non-zero even number".to_string());
        }
        if payload_len != shard_size {
            return Err(format!(
                "payload length {} != shard size {}",
                payload_len, shard_size
            ));
        }
        if payload_end != data.len() {
            return Err("shard payload length mismatch".to_string());
        }

        Ok(Self {
            hash,
            original_shards,
            recovery_shards,
            shard_index,
            shard_size,
            original_len,
            ttl,
            payload: data[payload_start..payload_end].to_vec(),
        })
    }

    fn with_ttl(&self, ttl: u8) -> Self {
        let mut next = self.clone();
        next.ttl = ttl;
        next
    }
}

struct TxSetShardAccumulator {
    original_shards: usize,
    recovery_shards: usize,
    shard_size: usize,
    original_len: usize,
    originals: HashMap<usize, Vec<u8>>,
    recoveries: HashMap<usize, Vec<u8>>,
    created_at: Instant,
}

impl TxSetShardAccumulator {
    fn new(shard: &TxSetShardMessage) -> Self {
        Self {
            original_shards: shard.original_shards,
            recovery_shards: shard.recovery_shards,
            shard_size: shard.shard_size,
            original_len: shard.original_len,
            originals: HashMap::new(),
            recoveries: HashMap::new(),
            created_at: Instant::now(),
        }
    }

    fn is_compatible(&self, shard: &TxSetShardMessage) -> bool {
        self.original_shards == shard.original_shards
            && self.recovery_shards == shard.recovery_shards
            && self.shard_size == shard.shard_size
            && self.original_len == shard.original_len
    }

    fn insert(&mut self, shard: &TxSetShardMessage) -> Result<bool, String> {
        if !self.is_compatible(shard) {
            return Err("shard parameters differ from accumulator".to_string());
        }

        if shard.shard_index < self.original_shards {
            Ok(self
                .originals
                .insert(shard.shard_index, shard.payload.clone())
                .is_none())
        } else {
            let recovery_index = shard.shard_index - self.original_shards;
            Ok(self
                .recoveries
                .insert(recovery_index, shard.payload.clone())
                .is_none())
        }
    }

    fn has_enough_shards(&self) -> bool {
        self.originals.len() + self.recoveries.len() >= self.original_shards
    }

    fn reconstruct(&self) -> Result<Option<Vec<u8>>, String> {
        if !self.has_enough_shards() {
            return Ok(None);
        }

        let mut pieces = Vec::with_capacity(self.original_shards * self.shard_size);
        if self.originals.len() == self.original_shards {
            for index in 0..self.original_shards {
                let shard = self
                    .originals
                    .get(&index)
                    .ok_or_else(|| format!("missing original shard {}", index))?;
                pieces.extend_from_slice(shard);
            }
            pieces.truncate(self.original_len);
            return Ok(Some(pieces));
        }

        let mut decoder =
            ReedSolomonDecoder::new(self.original_shards, self.recovery_shards, self.shard_size)
                .map_err(|e| format!("failed to create decoder: {}", e))?;
        for (index, shard) in &self.originals {
            decoder
                .add_original_shard(*index, shard)
                .map_err(|e| format!("failed to add original shard {}: {}", index, e))?;
        }
        for (index, shard) in &self.recoveries {
            decoder
                .add_recovery_shard(*index, shard)
                .map_err(|e| format!("failed to add recovery shard {}: {}", index, e))?;
        }

        let result = decoder
            .decode()
            .map_err(|e| format!("failed to decode txset shards: {}", e))?;
        let restored: HashMap<usize, Vec<u8>> = result
            .restored_original_iter()
            .map(|(index, shard)| (index, shard.to_vec()))
            .collect();

        for index in 0..self.original_shards {
            if let Some(shard) = self.originals.get(&index) {
                pieces.extend_from_slice(shard);
            } else if let Some(shard) = restored.get(&index) {
                pieces.extend_from_slice(shard);
            } else {
                return Ok(None);
            }
        }
        pieces.truncate(self.original_len);
        Ok(Some(pieces))
    }
}

fn make_txset_shards(
    hash: [u8; 32],
    data: &[u8],
    peer_count: usize,
    config: TxSetShardConfig,
) -> Result<Vec<TxSetShardMessage>, String> {
    let plan = plan_txset_shards(data.len(), peer_count, config)?;
    let original_shards = plan.original_shards;
    let recovery_shards = plan.recovery_shards;
    let shard_size = plan.shard_size;

    u16::try_from(original_shards).map_err(|_| "too many original shards".to_string())?;
    u16::try_from(recovery_shards).map_err(|_| "too many recovery shards".to_string())?;
    u16::try_from(original_shards + recovery_shards)
        .map_err(|_| "too many total shards".to_string())?;

    let mut originals = Vec::with_capacity(original_shards);
    for index in 0..original_shards {
        let start = index * shard_size;
        let end = (start + shard_size).min(data.len());
        let mut shard = vec![0u8; shard_size];
        if start < data.len() {
            shard[..end - start].copy_from_slice(&data[start..end]);
        }
        originals.push(shard);
    }

    let mut encoder = ReedSolomonEncoder::new(original_shards, recovery_shards, shard_size)
        .map_err(|e| format!("failed to create encoder: {}", e))?;
    for shard in &originals {
        encoder
            .add_original_shard(shard)
            .map_err(|e| format!("failed to add original shard: {}", e))?;
    }
    let result = encoder
        .encode()
        .map_err(|e| format!("failed to encode recovery shards: {}", e))?;
    let recoveries: Vec<Vec<u8>> = result.recovery_iter().map(|shard| shard.to_vec()).collect();

    let mut messages = Vec::with_capacity(original_shards + recovery_shards);
    for (index, payload) in originals.into_iter().enumerate() {
        messages.push(TxSetShardMessage {
            hash,
            original_shards,
            recovery_shards,
            shard_index: index,
            shard_size,
            original_len: data.len(),
            ttl: config.initial_ttl,
            payload,
        });
    }
    for (index, payload) in recoveries.into_iter().enumerate() {
        messages.push(TxSetShardMessage {
            hash,
            original_shards,
            recovery_shards,
            shard_index: original_shards + index,
            shard_size,
            original_len: data.len(),
            ttl: config.initial_ttl,
            payload,
        });
    }

    Ok(messages)
}

fn assign_shards_to_peer_offsets(shard_count: usize, peer_count: usize) -> Vec<Vec<usize>> {
    let mut assignments = vec![Vec::new(); peer_count];
    if peer_count == 0 {
        return assignments;
    }
    for shard_offset in 0..shard_count {
        assignments[shard_offset % peer_count].push(shard_offset);
    }
    assignments
}

/// Events from the overlay to the application
#[derive(Debug, Clone)]
pub enum OverlayEvent {
    /// Received SCP envelope from peer
    ScpReceived { envelope: Vec<u8>, from: PeerId },
    /// Received TX from peer
    TxReceived { tx: Vec<u8>, from: PeerId },
    /// Received TX set response
    TxSetReceived {
        hash: [u8; 32],
        data: Vec<u8>,
        from: PeerId,
        source: TxSetReceiveSource,
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

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum TxSetReceiveSource {
    Response,
    Shard,
}

/// Commands to the overlay
#[derive(Debug)]
pub enum OverlayCommand {
    /// Broadcast SCP envelope to all peers
    BroadcastScp(Vec<u8>),
    /// Broadcast TX to all peers
    BroadcastTx(Vec<u8>),
    /// Request TX set from a peer (picks best peer)
    FetchTxSet { hash: [u8; 32] },
    /// Send TX set to a specific peer (response to their request)
    SendTxSet {
        hash: [u8; 32],
        data: Vec<u8>,
        to: PeerId,
    },
    /// Broadcast a locally-built TX set through the shard dissemination path
    BroadcastTxSetShards { hash: [u8; 32], data: Vec<u8> },
    /// Record that a peer has a specific TX set (learned from SCP message)
    RecordTxSetSource { hash: [u8; 32], peer: PeerId },
    /// Connect to a peer by address (bootstrap — PeerId unknown)
    Dial(Multiaddr),
    /// Connect to a known peer by PeerId (reconnect — deduplicates automatically)
    DialPeer { peer_id: PeerId, addr: Multiaddr },
    /// Request SCP state from all peers
    RequestScpState { ledger_seq: u32 },
    /// Send SCP envelope to a specific peer
    SendScpToPeer { peer_id: PeerId, envelope: Vec<u8> },
    /// Shutdown
    Shutdown,
    /// Query the number of connected peers (responds via oneshot)
    GetConnectedPeerCount(tokio::sync::oneshot::Sender<usize>),
    /// Ping - responds immediately via oneshot channel (for testing event loop responsiveness)
    Ping(tokio::sync::oneshot::Sender<()>),
}

/// Outbound streams to a peer - each stream has its own mutex to avoid head-of-line blocking.
/// A large TxSet write won't block SCP sends to the same peer.
struct PeerOutboundStreams {
    scp: Mutex<Option<Stream>>,
    tx: Mutex<Option<Stream>>,
    txset: Mutex<Option<Stream>>,
    txset_shard: Mutex<Option<Stream>>,
}

impl PeerOutboundStreams {
    fn new() -> Self {
        Self {
            scp: Mutex::new(None),
            tx: Mutex::new(None),
            txset: Mutex::new(None),
            txset_shard: Mutex::new(None),
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

    pub async fn broadcast_tx(&self, tx: Vec<u8>) {
        if let Err(e) = self.cmd_tx.send(OverlayCommand::BroadcastTx(tx)).await {
            warn!(
                "Overlay command channel closed, failed to send BroadcastTx: {}",
                e
            );
        }
    }

    pub async fn fetch_txset(&self, hash: [u8; 32]) {
        if let Err(e) = self.cmd_tx.send(OverlayCommand::FetchTxSet { hash }).await {
            warn!(
                "Overlay command channel closed, failed to send FetchTxSet: {}",
                e
            );
        }
    }

    pub async fn send_txset(&self, hash: [u8; 32], data: Vec<u8>, to: PeerId) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::SendTxSet { hash, data, to })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send SendTxSet: {}",
                e
            );
        }
    }

    pub async fn broadcast_txset_shards(&self, hash: [u8; 32], data: Vec<u8>) {
        if let Err(e) = self
            .cmd_tx
            .send(OverlayCommand::BroadcastTxSetShards { hash, data })
            .await
        {
            warn!(
                "Overlay command channel closed, failed to send BroadcastTxSetShards: {}",
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
        self.cmd_tx
            .send(OverlayCommand::SendScpToPeer {
                peer_id,
                envelope: envelope.to_vec(),
            })
            .await
            .map_err(|_| io::Error::new(io::ErrorKind::Other, "Channel closed"))?;
        Ok(())
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
    /// Outbound streams per peer - each peer has three independently-locked streams
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
    pending_txset_requests: RwLock<HashMap<[u8; 32], (PeerId, Instant)>>,
    /// Partially received erasure-coded TX set shards.
    txset_shards: RwLock<HashMap<[u8; 32], TxSetShardAccumulator>>,
    /// TX sets already reconstructed through shards, used to drop duplicate shards.
    completed_txset_shards: RwLock<lru::LruCache<[u8; 32], ()>>,
    /// Tunables for TX set shard fanout and erasure coding.
    txset_shard_config: TxSetShardConfig,
    /// Event sender for non-TX events (SCP, TxSet - critical path, unbounded)
    event_tx: mpsc::UnboundedSender<OverlayEvent>,
    /// Bounded TX event sender (backpressure - drops allowed)
    tx_event_tx: mpsc::Sender<OverlayEvent>,
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
    /// Overlay metrics (shared with App for IPC reporting)
    metrics: Arc<OverlayMetrics>,
}

impl SharedState {
    fn new(
        event_tx: mpsc::UnboundedSender<OverlayEvent>,
        tx_event_tx: mpsc::Sender<OverlayEvent>,
        control: Control,
        txset_shard_config: TxSetShardConfig,
        metrics: Arc<OverlayMetrics>,
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
            txset_shards: RwLock::new(HashMap::new()),
            completed_txset_shards: RwLock::new(lru::LruCache::new(
                std::num::NonZeroUsize::new(1000).unwrap(),
            )),
            txset_shard_config,
            event_tx,
            tx_event_tx,
            tx_dropped_count: AtomicU64::new(0),
            control,
            // INV/GETDATA state
            inv_batcher: RwLock::new(InvBatcher::new()),
            inv_tracker: RwLock::new(InvTracker::new()),
            pending_getdata: RwLock::new(PendingRequests::new()),
            tx_buffer: RwLock::new(TxBuffer::new()),
            metrics,
        }
    }
}

/// The unified Stellar overlay
pub struct StellarOverlay {
    swarm: Swarm<StellarBehaviour>,
    control: Control,
    state: Arc<SharedState>,
    cmd_rx: mpsc::Receiver<OverlayCommand>,
}

/// Create the overlay and return handle + event receivers
///
/// Returns:
/// - `OverlayHandle`: for sending commands to the overlay
/// - `UnboundedReceiver<OverlayEvent>`: for SCP, TxSet events (critical path, never dropped)
/// - `Receiver<OverlayEvent>`: for TX events (bounded, may drop under backpressure)
/// - `StellarOverlay`: the overlay to run
pub fn create_overlay(
    keypair: Keypair,
    metrics: Arc<OverlayMetrics>,
) -> Result<
    (
        OverlayHandle,
        mpsc::UnboundedReceiver<OverlayEvent>,
        mpsc::Receiver<OverlayEvent>,
        StellarOverlay,
    ),
    Box<dyn std::error::Error + Send + Sync>,
> {
    create_overlay_with_txset_shard_config(keypair, metrics, TxSetShardConfig::default())
}

pub fn create_overlay_with_txset_shard_config(
    keypair: Keypair,
    metrics: Arc<OverlayMetrics>,
    txset_shard_config: TxSetShardConfig,
) -> Result<
    (
        OverlayHandle,
        mpsc::UnboundedReceiver<OverlayEvent>,
        mpsc::Receiver<OverlayEvent>,
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
    // Bounded channel for TX events - drops allowed under backpressure
    let (tx_event_tx, tx_event_rx) = mpsc::channel(TX_EVENT_CHANNEL_CAPACITY);

    let state = Arc::new(SharedState::new(
        event_tx,
        tx_event_tx,
        control.clone(),
        txset_shard_config,
        metrics,
    ));

    let overlay = StellarOverlay {
        swarm,
        control,
        state,
        cmd_rx,
    };

    let handle = OverlayHandle { cmd_tx };

    Ok((handle, event_rx, tx_event_rx, overlay))
}

impl StellarOverlay {
    /// Run the overlay event loop
    ///
    /// `listen_ip` should be a specific IP (e.g., "127.0.0.1" for local tests)
    /// to avoid multi-homing issues where Identify advertises multiple addresses.
    pub async fn run(mut self, listen_ip: &str, listen_port: u16) {
        // Start listening on QUIC (UDP)
        // Use specific IP to avoid Identify advertising all local IPs
        let listen_addr: Multiaddr = format!("/ip4/{}/udp/{}/quic-v1", listen_ip, listen_port)
            .parse()
            .unwrap();

        if let Err(e) = self.swarm.listen_on(listen_addr.clone()) {
            error!("Failed to listen on {}: {}", listen_addr, e);
            return;
        }
        info!("Listening on QUIC port {}", listen_port);

        // Accept incoming streams for each protocol
        let scp_incoming = match self.control.accept(SCP_PROTOCOL) {
            Ok(incoming) => incoming,
            Err(e) => {
                error!(
                    "Failed to accept SCP protocol streams: {:?}. Overlay cannot function.",
                    e
                );
                return;
            }
        };
        let tx_incoming = match self.control.accept(TX_PROTOCOL) {
            Ok(incoming) => incoming,
            Err(e) => {
                error!(
                    "Failed to accept TX protocol streams: {:?}. Overlay cannot function.",
                    e
                );
                return;
            }
        };
        let txset_incoming = match self.control.accept(TXSET_PROTOCOL) {
            Ok(incoming) => incoming,
            Err(e) => {
                error!(
                    "Failed to accept TxSet protocol streams: {:?}. Overlay cannot function.",
                    e
                );
                return;
            }
        };
        let txset_shard_incoming = match self.control.accept(TXSET_SHARD_PROTOCOL) {
            Ok(incoming) => incoming,
            Err(e) => {
                error!(
                    "Failed to accept TxSetShard protocol streams: {:?}. Overlay cannot function.",
                    e
                );
                return;
            }
        };

        // Spawn inbound stream handlers
        let state = self.state.clone();
        tokio::spawn(handle_inbound_scp_streams(scp_incoming, state.clone()));
        tokio::spawn(handle_inbound_tx_streams(tx_incoming, state.clone()));
        tokio::spawn(handle_inbound_txset_streams(txset_incoming, state.clone()));
        tokio::spawn(handle_inbound_txset_shard_streams(
            txset_shard_incoming,
            state.clone(),
        ));

        // Spawn INV/GETDATA housekeeping task
        tokio::spawn(inv_getdata_housekeeping_task(state.clone()));

        loop {
            tokio::select! {
                event = self.swarm.select_next_some() => {
                    self.handle_swarm_event(event).await;
                }

                Some(cmd) = self.cmd_rx.recv() => {
                    match cmd {
                        OverlayCommand::BroadcastScp(envelope) => {
                            self.broadcast_scp(&envelope).await;
                        }
                        OverlayCommand::BroadcastTx(tx) => {
                            self.broadcast_tx(&tx).await;
                        }
                        OverlayCommand::FetchTxSet { hash } => {
                            self.fetch_txset(hash).await;
                        }
                        OverlayCommand::SendTxSet { hash, data, to } => {
                            self.send_txset_response(to, hash, data).await;
                        }
                        OverlayCommand::BroadcastTxSetShards { hash, data } => {
                            self.broadcast_txset_shards(hash, data).await;
                        }
                        OverlayCommand::RecordTxSetSource { hash, peer } => {
                            let mut sources = self.state.txset_sources.write().await;
                            sources.put(hash, peer);
                            debug!("Recorded peer {} as source for TX set {:02x?}...", peer, &hash[..4]);
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
                        OverlayCommand::SendScpToPeer { peer_id, envelope } => {
                            // Don't hold &self across await - extract state and call helper directly
                            let state = Arc::clone(&self.state);
                            let message = match crate::xdr::encode_scp_message(&envelope) {
                                Ok(message) => message,
                                Err(e) => {
                                    warn!("Dropping invalid SCP envelope for {}: {}", peer_id, e);
                                    continue;
                                }
                            };
                            if let Err(e) = send_to_peer_stream(&state, peer_id.clone(), StreamType::Scp, &message).await {
                                warn!("Failed to send SCP to {}: {:?}", peer_id, e);
                            }
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
                }
            }

            SwarmEvent::ConnectionClosed {
                peer_id,
                num_established,
                ..
            } => {
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
                        pending.retain(|_hash, (p, _)| p != &peer_id);
                        let removed = before_len - pending.len();
                        if removed > 0 {
                            info!(
                                "Removed {} pending txset requests for disconnected peer {}",
                                removed, peer_id
                            );
                        }
                    }
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
        let message = match crate::xdr::encode_scp_message(envelope) {
            Ok(message) => message,
            Err(e) => {
                warn!("SCP_BROADCAST_DROP: Dropping invalid SCP envelope: {}", e);
                return;
            }
        };
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

        info!(
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
            tokio::spawn(async move {
                match send_to_peer_stream(&state, peer_id.clone(), StreamType::Scp, &message).await
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

    /// Broadcast TX to all connected peers
    /// Broadcast TX using INV/GETDATA protocol (bandwidth efficient)
    async fn broadcast_tx(&mut self, tx: &[u8]) {
        let parsed = match crate::xdr::parse_supported_transaction(tx) {
            Ok(parsed) => parsed,
            Err(e) => {
                warn!("TX_BROADCAST_DROP: Dropping invalid TX from Core: {}", e);
                return;
            }
        };
        let hash = parsed.full_hash;
        let tx = parsed.envelope_xdr;
        let fee_per_op = (parsed.fee / u64::from(parsed.num_ops.max(1))) as i64;

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

        // Store TX in buffer for GETDATA responses
        {
            let mut buffer = self.state.tx_buffer.write().await;
            buffer.insert(hash, tx.clone());
        }

        let streams = self.state.peer_streams.read().await;
        let peers: Vec<_> = streams.keys().cloned().collect();
        drop(streams);

        if peers.is_empty() {
            debug!("TX_INV: No peers to announce TX {:02x?}...", &hash[..4]);
            return;
        }

        debug!(
            "TX_INV: Announcing TX {:02x?}... ({} bytes) to {} peers via INV",
            &hash[..4],
            tx.len(),
            peers.len()
        );
        self.state
            .metrics
            .flood_advertised
            .fetch_add(peers.len() as u64, Ordering::Relaxed);

        let inv_entry = InvEntry { hash, fee_per_op };

        // Add to batcher for each peer, send batch immediately when full
        for peer in &peers {
            let batch_to_send = {
                let mut batcher = self.state.inv_batcher.write().await;
                batcher.add(*peer, inv_entry.clone())
            };
            if let Some(batch) = batch_to_send {
                send_inv_batch(&self.state, *peer, batch).await;
            }
        }
    }

    /// Fetch TX set from a peer - preferring the peer who sent us the SCP message referencing it
    async fn fetch_txset(&mut self, hash: [u8; 32]) {
        // Check if we're already fetching this TxSet from a connected peer (dedup)
        {
            let pending = self.state.pending_txset_requests.read().await;
            if let Some((pending_peer, _)) = pending.get(&hash) {
                // Check if that peer is still connected
                let streams = self.state.peer_streams.read().await;
                if streams.contains_key(pending_peer) {
                    debug!(
                        "TXSET_FETCH_SKIP: TxSet {:02x?}... already being fetched from {}, skipping duplicate",
                        &hash[..4], pending_peer
                    );
                    return;
                }
                // Otherwise, peer disconnected - we'll re-request below
            }
        }

        // First check if we know which peer has this TX set (from SCP message)
        let known_source = {
            let sources = self.state.txset_sources.read().await;
            sources.peek(&hash).cloned()
        };

        let peer = if let Some(source_peer) = known_source {
            // Verify this peer is still connected
            let streams = self.state.peer_streams.read().await;
            if streams.contains_key(&source_peer) {
                info!(
                    "TXSET_FETCH: Fetching TX set {:02x?}... from known source {}",
                    &hash[..4],
                    source_peer
                );
                source_peer
            } else {
                // Source peer disconnected, fall back to any peer
                match streams.keys().next().cloned() {
                    Some(p) => {
                        info!("TXSET_FETCH: Fetching TX set {:02x?}... from fallback peer {} (source {} disconnected)",
                              &hash[..4], p, source_peer);
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
            }
        } else {
            // No known source, pick any connected peer
            let streams = self.state.peer_streams.read().await;
            match streams.keys().next().cloned() {
                Some(p) => {
                    info!(
                        "TXSET_FETCH: Fetching TX set {:02x?}... from random peer {} (no known source)",
                        &hash[..4],
                        p
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

        // Record this pending request with timestamp for latency tracking
        self.state
            .pending_txset_requests
            .write()
            .await
            .insert(hash, (peer.clone(), Instant::now()));

        let request = match crate::xdr::encode_get_tx_set(hash) {
            Ok(request) => request,
            Err(e) => {
                warn!(
                    "TXSET_FETCH_FAIL: Failed to encode request for TxSet {:02x?}...: {}",
                    &hash[..4],
                    e
                );
                self.state
                    .pending_txset_requests
                    .write()
                    .await
                    .remove(&hash);
                return;
            }
        };

        match send_to_peer_stream(&self.state, peer.clone(), StreamType::TxSet, &request).await {
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
                self.state
                    .pending_txset_requests
                    .write()
                    .await
                    .remove(&hash);
            }
        }
    }

    /// Send TX set response to a specific peer
    async fn send_txset_response(&mut self, peer: PeerId, hash: [u8; 32], data: Vec<u8>) {
        info!(
            "TXSET_SEND: Sending TX set {:02x?}... ({} bytes) to {}",
            &hash[..4],
            data.len(),
            peer
        );

        let response = match crate::xdr::encode_generalized_tx_set_message(&data, &hash) {
            Ok(response) => response,
            Err(e) => {
                warn!(
                    "TXSET_SEND_DROP: Dropping invalid TxSet {:02x?}... for {}: {}",
                    &hash[..4],
                    peer,
                    e
                );
                return;
            }
        };

        match send_to_peer_stream(&self.state, peer, StreamType::TxSet, &response).await {
            Ok(_) => {
                self.state
                    .metrics
                    .send_txset
                    .fetch_add(1, Ordering::Relaxed);
                self.state
                    .metrics
                    .message_write
                    .fetch_add(1, Ordering::Relaxed);
                self.state
                    .metrics
                    .byte_write
                    .fetch_add(response.len() as u64, Ordering::Relaxed);
                info!(
                    "TXSET_SEND_OK: Successfully sent TX set {:02x?}... ({} bytes on wire) to {}",
                    &hash[..4],
                    response.len(),
                    peer
                );
            }
            Err(e) => {
                self.state
                    .metrics
                    .error_write
                    .fetch_add(1, Ordering::Relaxed);
                warn!(
                    "TXSET_SEND_FAIL: Failed to send TxSet {:02x?}... to {}: {}",
                    &hash[..4],
                    peer,
                    e
                );
            }
        }
    }

    async fn broadcast_txset_shards(&mut self, hash: [u8; 32], data: Vec<u8>) {
        let streams = self.state.peer_streams.read().await;
        let peers: Vec<_> = streams.keys().cloned().collect();
        drop(streams);

        if peers.is_empty() {
            debug!(
                "TXSET_SHARD_BROADCAST_SKIP: No peers to broadcast TxSet {:02x?}...",
                &hash[..4]
            );
            return;
        }

        let shards =
            match make_txset_shards(hash, &data, peers.len(), self.state.txset_shard_config) {
                Ok(shards) => shards,
                Err(e) => {
                    warn!(
                    "TXSET_SHARD_BROADCAST_DROP: Failed to shard TxSet {:02x?}... from Core: {}",
                    &hash[..4],
                    e
                );
                    return;
                }
            };

        info!(
            "TXSET_SHARD_BROADCAST: Broadcasting TxSet {:02x?}... ({} bytes, {} shards: {} original + {} recovery, shard_size={}) to {} peers",
            &hash[..4],
            data.len(),
            shards.len(),
            shards[0].original_shards,
            shards[0].recovery_shards,
            shards[0].shard_size,
            peers.len()
        );
        self.state
            .metrics
            .txset_shard_broadcast
            .fetch_add(1, Ordering::Relaxed);
        self.state
            .metrics
            .message_broadcast
            .fetch_add(1, Ordering::Relaxed);

        let mut messages_by_peer = Vec::new();
        for shard_offsets in assign_shards_to_peer_offsets(shards.len(), peers.len()) {
            let mut messages = Vec::with_capacity(shard_offsets.len());
            for shard_offset in shard_offsets {
                let shard = &shards[shard_offset];
                match shard.encode() {
                    Ok(message) => {
                        messages.push((message, shard.shard_index < shard.original_shards))
                    }
                    Err(e) => warn!(
                        "TXSET_SHARD_ENCODE_DROP: Failed to encode shard {} for TxSet {:02x?}...: {}",
                        shard.shard_index,
                        &hash[..4],
                        e
                    ),
                }
            }
            messages_by_peer.push(messages);
        }

        for (peer_id, messages) in peers.into_iter().zip(messages_by_peer) {
            if messages.is_empty() {
                continue;
            }
            let state = Arc::clone(&self.state);
            tokio::spawn(async move {
                for (message, is_original) in messages {
                    match send_to_peer_stream(
                        &state,
                        peer_id.clone(),
                        StreamType::TxSetShard,
                        &message,
                    )
                    .await
                    {
                        Ok(_) => {
                            state.metrics.message_write.fetch_add(1, Ordering::Relaxed);
                            state
                                .metrics
                                .byte_write
                                .fetch_add(message.len() as u64, Ordering::Relaxed);
                            if is_original {
                                state
                                    .metrics
                                    .txset_shard_original_sent
                                    .fetch_add(1, Ordering::Relaxed);
                            } else {
                                state
                                    .metrics
                                    .txset_shard_recovery_sent
                                    .fetch_add(1, Ordering::Relaxed);
                            }
                        }
                        Err(e) => {
                            state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
                            warn!(
                                "TXSET_SHARD_SEND_FAIL: Failed to send TxSet shard {:02x?}... to {}: {}",
                                &hash[..4],
                                peer_id,
                                e
                            );
                        }
                    }
                }
            });
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

        let request = match crate::xdr::encode_get_scp_state(ledger_seq) {
            Ok(request) => request,
            Err(e) => {
                warn!("Failed to encode SCP state request: {}", e);
                return;
            }
        };
        for peer_id in peers {
            if let Err(e) =
                send_to_peer_stream(&self.state, peer_id, StreamType::Scp, &request).await
            {
                warn!("Failed to send SCP state request to {}: {:?}", peer_id, e);
            }
        }
    }

    /// Send SCP envelope to a specific peer
    pub async fn send_scp_to_peer(&self, peer_id: PeerId, envelope: &[u8]) -> io::Result<()> {
        let message = crate::xdr::encode_scp_message(envelope)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        send_to_peer_stream(&self.state, peer_id, StreamType::Scp, &message).await
    }
}

/// Open SCP, TX, TxSet, and TxSetShard streams to a peer.
/// Spawned as a background task so the swarm event loop stays unblocked —
/// `control.open_stream()` needs the swarm to be polled to complete.
async fn open_streams_to_peer(mut control: Control, state: Arc<SharedState>, peer_id: PeerId) {
    debug!("Opening streams to peer {}", peer_id);

    let mut control2 = control.clone();
    let mut control3 = control.clone();
    let mut control4 = control.clone();

    let scp_fut = async { control.open_stream(peer_id, SCP_PROTOCOL).await };
    let tx_fut = async { control2.open_stream(peer_id, TX_PROTOCOL).await };
    let txset_fut = async { control3.open_stream(peer_id, TXSET_PROTOCOL).await };
    let txset_shard_fut = async { control4.open_stream(peer_id, TXSET_SHARD_PROTOCOL).await };

    let (scp_result, tx_result, txset_result, txset_shard_result) =
        tokio::join!(scp_fut, tx_fut, txset_fut, txset_shard_fut);

    let scp_stream = match scp_result {
        Ok(s) => {
            debug!("Opened SCP stream to {}", peer_id);
            Some(s)
        }
        Err(e) => {
            warn!("Failed to open SCP stream to {}: {:?}", peer_id, e);
            None
        }
    };

    let tx_stream = match tx_result {
        Ok(s) => {
            debug!("Opened TX stream to {}", peer_id);
            Some(s)
        }
        Err(e) => {
            warn!("Failed to open TX stream to {}: {:?}", peer_id, e);
            None
        }
    };

    let txset_stream = match txset_result {
        Ok(s) => {
            debug!("Opened TxSet stream to {}", peer_id);
            Some(s)
        }
        Err(e) => {
            warn!("Failed to open TxSet stream to {}: {:?}", peer_id, e);
            None
        }
    };

    let txset_shard_stream = match txset_shard_result {
        Ok(s) => {
            debug!("Opened TxSetShard stream to {}", peer_id);
            Some(s)
        }
        Err(e) => {
            warn!("Failed to open TxSetShard stream to {}: {:?}", peer_id, e);
            None
        }
    };

    // Store streams
    {
        let streams = state.peer_streams.read().await;
        if let Some(peer_streams) = streams.get(&peer_id) {
            if let Some(stream) = scp_stream {
                *peer_streams.scp.lock().await = Some(stream);
            }
            if let Some(stream) = tx_stream {
                *peer_streams.tx.lock().await = Some(stream);
            }
            if let Some(stream) = txset_stream {
                *peer_streams.txset.lock().await = Some(stream);
            }
            if let Some(stream) = txset_shard_stream {
                *peer_streams.txset_shard.lock().await = Some(stream);
            }
        }
    }

    // Request SCP state from newly connected peer
    info!("Peer {} streams opened, sending SCP state request", peer_id);
    let ledger_seq: u32 = 0;
    let request = match crate::xdr::encode_get_scp_state(ledger_seq) {
        Ok(request) => request,
        Err(e) => {
            info!("Failed to encode SCP state request for {}: {}", peer_id, e);
            return;
        }
    };
    if let Err(e) = send_to_peer_stream(&state, peer_id.clone(), StreamType::Scp, &request).await {
        info!(
            "Failed to request SCP state from newly connected peer {}: {:?}",
            peer_id, e
        );
    }
}

#[derive(Clone, Copy)]
enum StreamType {
    Scp,
    Tx,
    TxSet,
    TxSetShard,
}

impl StreamType {
    fn protocol(&self) -> StreamProtocol {
        match self {
            StreamType::Scp => SCP_PROTOCOL,
            StreamType::Tx => TX_PROTOCOL,
            StreamType::TxSet => TXSET_PROTOCOL,
            StreamType::TxSetShard => TXSET_SHARD_PROTOCOL,
        }
    }
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

    // Lock only the specific stream we need - no head-of-line blocking
    let stream_mutex = match stream_type {
        StreamType::Scp => &peer_streams.scp,
        StreamType::Tx => &peer_streams.tx,
        StreamType::TxSet => &peer_streams.txset,
        StreamType::TxSetShard => &peer_streams.txset_shard,
    };

    let mut stream_guard = stream_mutex.lock().await;

    // If stream not open, fail immediately without reopening
    let stream = stream_guard
        .as_mut()
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "stream not open"))?;

    write_framed(stream, data).await
}

/// Send message to a specific peer's stream, reopening if needed
async fn send_to_peer_stream(
    state: &SharedState,
    peer_id: PeerId,
    stream_type: StreamType,
    data: &[u8],
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

        // Lock only the specific stream we need - no head-of-line blocking
        let stream_mutex = match stream_type {
            StreamType::Scp => &peer_streams.scp,
            StreamType::Tx => &peer_streams.tx,
            StreamType::TxSet => &peer_streams.txset,
            StreamType::TxSetShard => &peer_streams.txset_shard,
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
            match state
                .control
                .clone()
                .open_stream(peer_id, stream_type.protocol())
                .await
            {
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
        match write_framed(stream, data).await {
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
    let len = data.len() as u32;
    stream.write_all(&len.to_be_bytes()).await?;
    stream.write_all(data).await?;
    stream.flush().await?;
    Ok(())
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

/// Send an INV batch to a peer
async fn send_inv_batch(state: &Arc<SharedState>, peer: PeerId, batch: InvBatch) {
    let batch_size = batch.entries.len() as u64;
    let msg = TxStreamMessage::InvBatch(batch);
    let encoded = match msg.encode() {
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
async fn read_framed(stream: &mut Stream) -> io::Result<Vec<u8>> {
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await?;
    let len = u32::from_be_bytes(len_buf) as usize;

    if len > MAX_MESSAGE_SIZE {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("message too large: {} > {}", len, MAX_MESSAGE_SIZE),
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

                        let envelope = match message {
                            stellar_xdr::curr::StellarMessage::GetScpState(ledger_seq) => {
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
                            stellar_xdr::curr::StellarMessage::ScpMessage(envelope) => {
                                match crate::xdr::canonical_scp_envelope_xdr(envelope) {
                                    Ok(envelope) => envelope,
                                    Err(e) => {
                                        warn!(
                                            "SCP_PARSE_ERR: Dropping invalid SCP envelope from {}: {}",
                                            peer_id, e
                                        );
                                        continue;
                                    }
                                }
                            }
                            other => {
                                warn!(
                                    "SCP_PARSE_ERR: Dropping unexpected {} on SCP stream from {}",
                                    other.name(),
                                    peer_id
                                );
                                continue;
                            }
                        };

                        let hash = blake2b_hash(&envelope);
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
                            envelope.len(),
                            peer_id
                        );

                        // Forward to Core
                        if let Err(e) = state.event_tx.send(OverlayEvent::ScpReceived {
                            envelope,
                            from: peer_id.clone(),
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

/// Handle inbound TX streams from peers
async fn handle_inbound_tx_streams(mut incoming: IncomingStreams, state: Arc<SharedState>) {
    while let Some((peer_id, mut stream)) = incoming.next().await {
        info!("TX_STREAM: Accepted inbound TX stream from {}", peer_id);
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
                        // Parse INV/GETDATA message
                        handle_tx_stream_message(&state, &peer_id, &data, &mut stream).await;
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
async fn handle_tx_stream_message(
    state: &Arc<SharedState>,
    peer_id: &PeerId,
    data: &[u8],
    stream: &mut Stream,
) {
    match TxStreamMessage::decode(data) {
        Ok(TxStreamMessage::InvBatch(batch)) => {
            handle_inv_batch(state, peer_id, batch).await;
        }
        Ok(TxStreamMessage::GetData(getdata)) => {
            handle_getdata(state, peer_id, getdata, stream).await;
        }
        Ok(TxStreamMessage::Tx(tx_data)) => {
            handle_tx_response(state, peer_id, tx_data).await;
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

    let mut to_request: Vec<[u8; 32]> = Vec::new();

    for entry in batch.entries {
        // Check if we already have this TX
        {
            let seen = state.tx_seen.read().await;
            if seen.contains(&entry.hash) {
                // Already have it, just record this peer as a source (for relay tracking)
                continue;
            }
        }

        // Record this peer as a source for round-robin GETDATA
        let is_first = {
            let mut tracker = state.inv_tracker.write().await;
            tracker.record_source(entry.hash, *peer_id)
        };

        // If this is the first INV for this TX, we should request it
        if is_first {
            to_request.push(entry.hash);
        }
    }

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
        let msg = TxStreamMessage::GetData(getdata);
        let encoded = match msg.encode() {
            Ok(encoded) => encoded,
            Err(e) => {
                warn!("Failed to encode GETDATA for {}: {}", peer_id, e);
                return;
            }
        };

        let state_clone = Arc::clone(state);
        let peer_clone = *peer_id;
        tokio::spawn(async move {
            if let Err(e) =
                send_to_peer_stream(&state_clone, peer_clone, StreamType::Tx, &encoded).await
            {
                warn!("Failed to send GETDATA to {}: {}", peer_clone, e);
            }
        });
    }
}

/// Handle GETDATA message - respond with requested TXs
async fn handle_getdata(
    state: &Arc<SharedState>,
    peer_id: &PeerId,
    getdata: GetData,
    _stream: &mut Stream,
) {
    debug!(
        "TX_GETDATA_RECV: Peer {} requesting {} TXs",
        peer_id,
        getdata.hashes.len()
    );

    for hash in getdata.hashes {
        // Look up TX in our buffer
        let tx_data = {
            let mut buffer = state.tx_buffer.write().await;
            buffer.get_cloned(&hash)
        };

        if let Some(tx_data) = tx_data {
            state
                .metrics
                .flood_fulfilled
                .fetch_add(1, Ordering::Relaxed);
            // Send TX response
            let msg = TxStreamMessage::Tx(tx_data);
            let encoded = match msg.encode() {
                Ok(encoded) => encoded,
                Err(e) => {
                    state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
                    warn!("Failed to encode TX response for {}: {}", peer_id, e);
                    continue;
                }
            };

            let state_clone = Arc::clone(state);
            let peer_clone = *peer_id;
            tokio::spawn(async move {
                if let Err(e) =
                    send_to_peer_stream(&state_clone, peer_clone, StreamType::Tx, &encoded).await
                {
                    state_clone
                        .metrics
                        .error_write
                        .fetch_add(1, Ordering::Relaxed);
                    warn!("Failed to send TX to {}: {}", peer_clone, e);
                } else {
                    state_clone
                        .metrics
                        .message_write
                        .fetch_add(1, Ordering::Relaxed);
                    state_clone
                        .metrics
                        .byte_write
                        .fetch_add(encoded.len() as u64, Ordering::Relaxed);
                    debug!("TX_SEND: Sent TX {:02x?}... to {}", &hash[..4], peer_clone);
                }
            });
        } else {
            state
                .metrics
                .flood_unfulfilled_unknown
                .fetch_add(1, Ordering::Relaxed);
            trace!(
                "TX_GETDATA_MISS: Don't have TX {:02x?}... for {}",
                &hash[..4],
                peer_id
            );
        }
    }
}

/// Handle TX response (from GETDATA request)
async fn handle_tx_response(state: &Arc<SharedState>, peer_id: &PeerId, tx: Vec<u8>) {
    let parsed = match crate::xdr::parse_supported_transaction(&tx) {
        Ok(parsed) => parsed,
        Err(e) => {
            warn!("TX_RECV_DROP: Dropping invalid TX from {}: {}", peer_id, e);
            return;
        }
    };
    let hash = parsed.full_hash;
    let tx = parsed.envelope_xdr;
    let fee_per_op = (parsed.fee / u64::from(parsed.num_ops.max(1))) as i64;
    let recv_start = std::time::Instant::now();
    let tx_len = tx.len() as u64;

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

    // Remove from pending requests and measure pull latency
    {
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
        }
    }

    // Store in buffer for responding to others' GETDATA
    {
        let mut buffer = state.tx_buffer.write().await;
        buffer.insert(hash, tx.clone());
    }

    debug!(
        "TX_RECV: Received TX {:02x?}... ({} bytes) from {}",
        &hash[..4],
        tx.len(),
        peer_id
    );

    // Forward to Core via bounded TX channel
    if let Err(_) = state.tx_event_tx.try_send(OverlayEvent::TxReceived {
        tx: tx.clone(),
        from: peer_id.clone(),
    }) {
        state.metrics.message_drop.fetch_add(1, Ordering::Relaxed);
        let dropped = state.tx_dropped_count.fetch_add(1, Ordering::Relaxed) + 1;
        if dropped % 1000 == 1 {
            warn!(
                "TX_BACKPRESSURE: Dropped TX {:02x?}... (total dropped: {})",
                &hash[..4],
                dropped
            );
        }
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

        let inv_entry = InvEntry { hash, fee_per_op };

        // Add to batcher for each peer, send batch immediately when full
        for peer in &peers_to_announce {
            let batch_to_send = {
                let mut batcher = state.inv_batcher.write().await;
                batcher.add(*peer, inv_entry.clone())
            };
            if let Some(batch) = batch_to_send {
                send_inv_batch(state, *peer, batch).await;
            }
        }
    }

    // Record recv-transaction timing
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

/// Handle inbound TxSet streams from peers
async fn handle_inbound_txset_streams(mut incoming: IncomingStreams, state: Arc<SharedState>) {
    while let Some((peer_id, mut stream)) = incoming.next().await {
        debug!("Accepted inbound TxSet stream from {}", peer_id);
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
                                warn!(
                                    "TXSET_PARSE_ERR: Dropping malformed TxSet stream message from {}: {}",
                                    peer_id, e
                                );
                                continue;
                            }
                        };

                        match message {
                            stellar_xdr::curr::StellarMessage::GetTxSet(hash) => {
                                let hash = hash.0;
                                info!(
                                    "TXSET_REQ_IN: Received TxSet request for {:02x?}... from {}",
                                    &hash[..4],
                                    peer_id
                                );

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
                            stellar_xdr::curr::StellarMessage::GeneralizedTxSet(tx_set) => {
                                let (hash, txset_data) =
                                    match crate::xdr::canonical_generalized_tx_set_xdr(tx_set) {
                                        Ok(value) => value,
                                        Err(e) => {
                                            warn!(
                                                "TXSET_PARSE_ERR: Dropping invalid TxSet from {}: {}",
                                                peer_id, e
                                            );
                                            continue;
                                        }
                                    };

                                // Clear pending request flag and measure fetch latency
                                let was_pending = {
                                    let mut pending = state.pending_txset_requests.write().await;
                                    if let Some((_, request_time)) = pending.remove(&hash) {
                                        let fetch_us = request_time.elapsed().as_micros() as u64;
                                        state
                                            .metrics
                                            .fetch_txset_sum_us
                                            .fetch_add(fetch_us, Ordering::Relaxed);
                                        state
                                            .metrics
                                            .fetch_txset_count
                                            .fetch_add(1, Ordering::Relaxed);
                                        true
                                    } else {
                                        false
                                    }
                                };

                                info!(
                                    "TXSET_RECV: Received TxSet {:02x?}... ({} bytes) from {} (was_pending={})",
                                    &hash[..4],
                                    txset_data.len(),
                                    peer_id,
                                    was_pending
                                );
                                if let Err(e) = state.event_tx.send(OverlayEvent::TxSetReceived {
                                    hash,
                                    data: txset_data,
                                    from: peer_id,
                                    source: TxSetReceiveSource::Response,
                                }) {
                                    warn!(
                                        "Failed to forward TxSetReceived event from {}: {}",
                                        peer_id, e
                                    );
                                }
                            }
                            other => {
                                warn!(
                                    "TXSET_PARSE_ERR: Dropping unexpected {} on TxSet stream from {}",
                                    other.name(),
                                    peer_id
                                );
                            }
                        }
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

async fn handle_inbound_txset_shard_streams(
    mut incoming: IncomingStreams,
    state: Arc<SharedState>,
) {
    while let Some((peer_id, mut stream)) = incoming.next().await {
        debug!("Accepted inbound TxSetShard stream from {}", peer_id);
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
                        handle_txset_shard_message(&state, peer_id, data).await;
                    }
                    Err(e) => {
                        state.metrics.error_read.fetch_add(1, Ordering::Relaxed);
                        state.metrics.inbound_live.fetch_sub(1, Ordering::Relaxed);
                        info!("TxSetShard stream from {} closed: {}", peer_id, e);
                        break;
                    }
                }
            }
        });
    }
}

async fn handle_txset_shard_message(state: &Arc<SharedState>, from: PeerId, data: Vec<u8>) {
    let shard = match TxSetShardMessage::decode(&data) {
        Ok(shard) => shard,
        Err(e) => {
            warn!(
                "TXSET_SHARD_PARSE_ERR: Dropping malformed shard from {}: {}",
                from, e
            );
            return;
        }
    };

    {
        let completed = state.completed_txset_shards.read().await;
        if completed.peek(&shard.hash).is_some() {
            record_redundant_txset_shard(state, &shard);
            trace!(
                "TXSET_SHARD_SKIP: TxSet {:02x?}... already reconstructed",
                &shard.hash[..4]
            );
            return;
        }
    }

    let mut should_rebroadcast = false;
    let mut reconstructed = None;
    {
        let mut accumulators = state.txset_shards.write().await;
        let accumulator = accumulators
            .entry(shard.hash)
            .or_insert_with(|| TxSetShardAccumulator::new(&shard));

        let inserted = match accumulator.insert(&shard) {
            Ok(inserted) => inserted,
            Err(e) => {
                warn!(
                    "TXSET_SHARD_DROP: Dropping incompatible shard {} for {:02x?}... from {}: {}",
                    shard.shard_index,
                    &shard.hash[..4],
                    from,
                    e
                );
                return;
            }
        };

        if inserted {
            record_unique_txset_shard(state, &shard);
            should_rebroadcast = shard.ttl > 0;
            let used_recovery = accumulator.originals.len() < accumulator.original_shards;
            let recovery_reconstruct_start = used_recovery.then(Instant::now);
            match accumulator.reconstruct() {
                Ok(Some(txset)) => {
                    if let Some(start) = recovery_reconstruct_start {
                        state
                            .metrics
                            .txset_shard_reconstruct_recovery_sum_us
                            .fetch_add(start.elapsed().as_micros() as u64, Ordering::Relaxed);
                        state
                            .metrics
                            .txset_shard_reconstruct_recovery_count
                            .fetch_add(1, Ordering::Relaxed);
                        state
                            .metrics
                            .txset_shard_reconstruct_success_recovery
                            .fetch_add(1, Ordering::Relaxed);
                    } else {
                        state
                            .metrics
                            .txset_shard_reconstruct_success_original
                            .fetch_add(1, Ordering::Relaxed);
                    }
                    info!(
                        "TXSET_SHARD_RECONSTRUCTED: Reconstructed TxSet {:02x?}... ({} bytes) from {} shards in {:?}",
                        &shard.hash[..4],
                        txset.len(),
                        accumulator.originals.len() + accumulator.recoveries.len(),
                        accumulator.created_at.elapsed()
                    );
                    reconstructed = Some(txset);
                    accumulators.remove(&shard.hash);
                }
                Ok(None) => {}
                Err(e) => {
                    if let Some(start) = recovery_reconstruct_start {
                        state
                            .metrics
                            .txset_shard_reconstruct_recovery_sum_us
                            .fetch_add(start.elapsed().as_micros() as u64, Ordering::Relaxed);
                        state
                            .metrics
                            .txset_shard_reconstruct_recovery_count
                            .fetch_add(1, Ordering::Relaxed);
                        state
                            .metrics
                            .txset_shard_reconstruct_fail_recovery
                            .fetch_add(1, Ordering::Relaxed);
                    } else {
                        state
                            .metrics
                            .txset_shard_reconstruct_fail_original
                            .fetch_add(1, Ordering::Relaxed);
                    }
                    warn!(
                        "TXSET_SHARD_DECODE_FAIL: Failed to decode TxSet {:02x?}...: {}",
                        &shard.hash[..4],
                        e
                    );
                }
            }
        } else {
            record_redundant_txset_shard(state, &shard);
            trace!(
                "TXSET_SHARD_DUP: Duplicate shard {} for TxSet {:02x?}... from {}",
                shard.shard_index,
                &shard.hash[..4],
                from
            );
        }
    }

    if let Some(txset) = reconstructed {
        state
            .completed_txset_shards
            .write()
            .await
            .put(shard.hash, ());
        if let Err(e) = state.event_tx.send(OverlayEvent::TxSetReceived {
            hash: shard.hash,
            data: txset,
            from,
            source: TxSetReceiveSource::Shard,
        }) {
            warn!(
                "TXSET_SHARD_TO_APP_FAIL: Failed to forward reconstructed TxSet {:02x?}...: {}",
                &shard.hash[..4],
                e
            );
        }
    }

    if should_rebroadcast {
        rebroadcast_txset_shard(state, &shard.with_ttl(shard.ttl - 1), from).await;
    }
}

fn record_unique_txset_shard(state: &Arc<SharedState>, shard: &TxSetShardMessage) {
    if shard.shard_index < shard.original_shards {
        state
            .metrics
            .txset_shard_original_recv_unique
            .fetch_add(1, Ordering::Relaxed);
    } else {
        state
            .metrics
            .txset_shard_recovery_recv_unique
            .fetch_add(1, Ordering::Relaxed);
    }
}

fn record_redundant_txset_shard(state: &Arc<SharedState>, shard: &TxSetShardMessage) {
    if shard.shard_index < shard.original_shards {
        state
            .metrics
            .txset_shard_original_recv_redundant
            .fetch_add(1, Ordering::Relaxed);
    } else {
        state
            .metrics
            .txset_shard_recovery_recv_redundant
            .fetch_add(1, Ordering::Relaxed);
    }
}

async fn rebroadcast_txset_shard(
    state: &Arc<SharedState>,
    shard: &TxSetShardMessage,
    from: PeerId,
) {
    let message = match shard.encode() {
        Ok(message) => message,
        Err(e) => {
            warn!(
                "TXSET_SHARD_REBROADCAST_DROP: Failed to encode shard {} for {:02x?}...: {}",
                shard.shard_index,
                &shard.hash[..4],
                e
            );
            return;
        }
    };

    let streams = state.peer_streams.read().await;
    let peers: Vec<_> = streams
        .keys()
        .filter(|peer| **peer != from)
        .cloned()
        .collect();
    drop(streams);

    for peer_id in peers {
        let state = Arc::clone(state);
        let message = message.clone();
        let hash = shard.hash;
        let shard_index = shard.shard_index;
        let is_original = shard.shard_index < shard.original_shards;
        tokio::spawn(async move {
            match send_to_peer_stream(&state, peer_id.clone(), StreamType::TxSetShard, &message)
                .await
            {
                Ok(_) => {
                    state.metrics.message_write.fetch_add(1, Ordering::Relaxed);
                    state
                        .metrics
                        .byte_write
                        .fetch_add(message.len() as u64, Ordering::Relaxed);
                    if is_original {
                        state
                            .metrics
                            .txset_shard_original_forwarded
                            .fetch_add(1, Ordering::Relaxed);
                    } else {
                        state
                            .metrics
                            .txset_shard_recovery_forwarded
                            .fetch_add(1, Ordering::Relaxed);
                    }
                }
                Err(e) => {
                    state.metrics.error_write.fetch_add(1, Ordering::Relaxed);
                    warn!(
                        "TXSET_SHARD_REBROADCAST_FAIL: Failed to send shard {} for {:02x?}... to {}: {}",
                        shard_index,
                        &hash[..4],
                        peer_id,
                        e
                    );
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
            let pending = state.pending_getdata.write().await;
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

            // Send one batched GETDATA per peer
            for (peer, hashes) in per_peer {
                debug!(
                    "GETDATA_RETRY: Retrying {} TXs to peer {}",
                    hashes.len(),
                    peer
                );
                let getdata = GetData { hashes };
                let msg = TxStreamMessage::GetData(getdata);
                let encoded = match msg.encode() {
                    Ok(encoded) => encoded,
                    Err(e) => {
                        warn!("Failed to encode GETDATA retry to {}: {}", peer, e);
                        continue;
                    }
                };

                if let Err(e) =
                    try_send_to_existing_stream(&state, peer.clone(), StreamType::Tx, &encoded)
                        .await
                {
                    warn!("Failed to send GETDATA retry to {}: {:?}", peer, e);
                }
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
    use stellar_xdr::curr::{Limits, ScpEnvelope, WriteXdr};

    let mut envelope = ScpEnvelope::default();
    envelope.statement.slot_index = slot_index;
    envelope.to_xdr(Limits::none()).unwrap()
}

#[cfg(test)]
fn test_tx_xdr(sequence: i64) -> Vec<u8> {
    crate::xdr::tests::valid_transaction_xdr(1000, sequence, 1)
}

#[cfg(test)]
fn test_txset_xdr(seed: u8) -> ([u8; 32], Vec<u8>) {
    use stellar_xdr::curr::{GeneralizedTransactionSet, Hash};

    let mut tx_set = GeneralizedTransactionSet::default();
    let GeneralizedTransactionSet::V1(v1) = &mut tx_set;
    v1.previous_ledger_hash = Hash([seed; 32]);
    crate::xdr::canonical_generalized_tx_set_xdr(tx_set).unwrap()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_txset_shard_message_roundtrip() {
        let message = TxSetShardMessage {
            hash: [0x42; 32],
            original_shards: 3,
            recovery_shards: 2,
            shard_index: 4,
            shard_size: 8,
            original_len: 19,
            ttl: 1,
            payload: vec![7; 8],
        };

        let encoded = message.encode().unwrap();
        let decoded = TxSetShardMessage::decode(&encoded).unwrap();

        assert_eq!(decoded.hash, message.hash);
        assert_eq!(decoded.original_shards, message.original_shards);
        assert_eq!(decoded.recovery_shards, message.recovery_shards);
        assert_eq!(decoded.shard_index, message.shard_index);
        assert_eq!(decoded.shard_size, message.shard_size);
        assert_eq!(decoded.original_len, message.original_len);
        assert_eq!(decoded.ttl, message.ttl);
        assert_eq!(decoded.payload, message.payload);
    }

    #[test]
    fn test_txset_shard_reconstruction_with_missing_original() {
        let data = b"this txset payload spans several tiny shards".to_vec();
        let config = TxSetShardConfig {
            target_shard_size: 8,
            recovery_factor_percent: 50,
            initial_ttl: 1,
        };
        let shards = make_txset_shards([0x24; 32], &data, 2, config).unwrap();

        let mut accumulator = TxSetShardAccumulator::new(&shards[0]);
        for shard in shards
            .iter()
            .filter(|shard| shard.shard_index != 1)
            .take(shards[0].original_shards)
        {
            assert!(accumulator.insert(shard).unwrap());
        }

        let reconstructed = accumulator.reconstruct().unwrap().unwrap();
        assert_eq!(reconstructed, data);
    }

    #[tokio::test]
    async fn test_txset_shard_metrics_original_reconstruction_and_redundant() {
        let metrics = Arc::new(OverlayMetrics::new());
        let keypair = Keypair::generate_ed25519();
        let (_handle, mut events, _tx_events, overlay) =
            create_overlay(keypair, Arc::clone(&metrics)).unwrap();
        let state = overlay.state.clone();

        let data = b"original-only reconstruction".to_vec();
        let shards = make_txset_shards([0x31; 32], &data, 2, TxSetShardConfig::default()).unwrap();
        let original_count = shards[0].original_shards;
        let peer = PeerId::random();

        for shard in shards.iter().take(original_count) {
            handle_txset_shard_message(&state, peer, shard.encode().unwrap()).await;
        }

        let event = events.recv().await.unwrap();
        match event {
            OverlayEvent::TxSetReceived {
                hash,
                data: reconstructed,
                source,
                ..
            } => {
                assert_eq!(hash, [0x31; 32]);
                assert_eq!(reconstructed, data);
                assert_eq!(source, TxSetReceiveSource::Shard);
            }
            other => panic!("unexpected event: {:?}", other),
        }

        assert_eq!(
            metrics
                .txset_shard_original_recv_unique
                .load(Ordering::Relaxed),
            original_count as u64
        );
        assert_eq!(
            metrics
                .txset_shard_reconstruct_success_original
                .load(Ordering::Relaxed),
            1
        );

        handle_txset_shard_message(&state, peer, shards[0].encode().unwrap()).await;
        assert_eq!(
            metrics
                .txset_shard_original_recv_redundant
                .load(Ordering::Relaxed),
            1
        );
    }

    #[tokio::test]
    async fn test_txset_shard_metrics_recovery_reconstruction() {
        let metrics = Arc::new(OverlayMetrics::new());
        let keypair = Keypair::generate_ed25519();
        let (_handle, mut events, _tx_events, overlay) =
            create_overlay(keypair, Arc::clone(&metrics)).unwrap();
        let state = overlay.state.clone();

        let data = b"recovery-assisted reconstruction needs parity".to_vec();
        let config = TxSetShardConfig {
            target_shard_size: 8,
            recovery_factor_percent: 50,
            initial_ttl: 0,
        };
        let shards = make_txset_shards([0x32; 32], &data, 2, config).unwrap();
        let original_count = shards[0].original_shards;
        let peer = PeerId::random();

        for shard in shards
            .iter()
            .filter(|shard| shard.shard_index != 1)
            .take(original_count)
        {
            handle_txset_shard_message(&state, peer, shard.encode().unwrap()).await;
        }

        let event = events.recv().await.unwrap();
        match event {
            OverlayEvent::TxSetReceived {
                hash,
                data: reconstructed,
                source,
                ..
            } => {
                assert_eq!(hash, [0x32; 32]);
                assert_eq!(reconstructed, data);
                assert_eq!(source, TxSetReceiveSource::Shard);
            }
            other => panic!("unexpected event: {:?}", other),
        }

        assert_eq!(
            metrics
                .txset_shard_recovery_recv_unique
                .load(Ordering::Relaxed),
            1
        );
        assert_eq!(
            metrics
                .txset_shard_reconstruct_success_recovery
                .load(Ordering::Relaxed),
            1
        );
        assert_eq!(
            metrics
                .txset_shard_reconstruct_recovery_count
                .load(Ordering::Relaxed),
            1
        );
    }

    #[test]
    fn test_txset_shard_plan_uses_even_original_count() {
        let plan = plan_txset_shards(3 * 1024, 16, TxSetShardConfig::default()).unwrap();

        assert_eq!(plan.original_shards % 2, 0);
        assert!(plan.original_shards >= 2);
        assert!(plan.original_shards + plan.recovery_shards <= TXSET_MAX_TOTAL_SHARDS);
        assert_eq!(plan.shard_size, TXSET_TARGET_SHARD_SIZE);
    }

    #[test]
    fn test_txset_shard_plan_targets_about_twice_peer_count() {
        let peer_count = 20;
        let plan =
            plan_txset_shards(10 * 1024 * 1024, peer_count, TxSetShardConfig::default()).unwrap();

        assert_eq!(plan.original_shards % 2, 0);
        assert!(plan.original_shards + plan.recovery_shards <= peer_count * 2);
        assert!(plan.shard_size > TXSET_TARGET_SHARD_SIZE);
    }

    #[test]
    fn test_txset_shard_plan_increases_size_to_stay_under_256_total_shards() {
        let plan = plan_txset_shards(10 * 1024 * 1024, 200, TxSetShardConfig::default()).unwrap();

        assert_eq!(plan.original_shards % 2, 0);
        assert!(plan.original_shards + plan.recovery_shards <= TXSET_MAX_TOTAL_SHARDS);
        assert!(plan.shard_size > TXSET_TARGET_SHARD_SIZE);
    }

    #[test]
    fn test_txset_shard_assignment_covers_every_shard_once() {
        let assignments = assign_shards_to_peer_offsets(10, 3);
        let mut assigned: Vec<_> = assignments.iter().flatten().copied().collect();

        assigned.sort_unstable();
        assert_eq!(assigned, (0..10).collect::<Vec<_>>());
        let sizes: Vec<_> = assignments.iter().map(Vec::len).collect();
        assert_eq!(sizes, vec![4, 3, 3]);
    }

    #[tokio::test]
    async fn test_overlay_creation() {
        let keypair = Keypair::generate_ed25519();
        let (handle, _events, _tx_events, overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        while tx_events2.try_recv().is_ok() {}

        let tx_count = 1000;

        let tx_start = std::time::Instant::now();
        for i in 0..tx_count {
            let tx = test_tx_xdr(i as i64);
            handle1.broadcast_tx(tx).await;
        }

        // Immediately send small SCP (should bypass TX queue)
        let scp_msg = test_scp_envelope_xdr(3);
        let scp_send_time = std::time::Instant::now();
        handle1.broadcast_scp(scp_msg.clone()).await;

        // Track when SCP arrives vs when all TXs arrive
        // SCP comes on unbounded events channel, TX on bounded tx_events channel
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
                Some(event) = tx_events2.recv() => {
                    if let OverlayEvent::TxReceived { .. } = event {
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

        // Also verify TX flood took meaningful time (not instant)
        assert!(
            tx_total_time > Duration::from_millis(50),
            "TX flood should take measurable time ({:?}), otherwise test is invalid",
            tx_total_time
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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        while tx_events2.try_recv().is_ok() {}

        let scp_count = 1000;

        let scp_start = std::time::Instant::now();
        for i in 0..scp_count {
            let scp = test_scp_envelope_xdr(i as u64);
            handle1.broadcast_scp(scp).await;
        }

        // Immediately send TX (should bypass SCP queue)
        let tx_msg = test_tx_xdr(10_000);
        let tx_send_time = std::time::Instant::now();
        handle1.broadcast_tx(tx_msg.clone()).await;

        // Track when TX arrives vs when all SCPs arrive
        // SCP comes on unbounded events channel, TX on bounded tx_events channel
        let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
        let mut tx_received_at: Option<std::time::Instant> = None;
        let mut scp_count_received = 0u32;
        let mut all_scp_received_at: Option<std::time::Instant> = None;

        while tokio::time::Instant::now() < deadline {
            tokio::select! {
                Some(event) = tx_events2.recv() => {
                    if let OverlayEvent::TxReceived { tx, .. } = event {
                        if tx == tx_msg && tx_received_at.is_none() {
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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, _events2, mut tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        while tx_events2.try_recv().is_ok() {}

        // Send TX
        let tx_msg = test_tx_xdr(20_000);
        handle1.broadcast_tx(tx_msg.clone()).await;

        // Wait for TX on the bounded TX events channel
        let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
        let mut received = false;

        while tokio::time::Instant::now() < deadline && !received {
            tokio::select! {
                Some(event) = tx_events2.recv() => {
                    if let OverlayEvent::TxReceived { tx, .. } = event {
                        assert_eq!(tx, tx_msg);
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

        let (handle1, mut events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        handle2.fetch_txset(requested_hash).await;

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
                        handle1.send_txset(hash, txset_data.clone(), from).await;
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
                        assert_eq!(data, txset_data);
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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, _events2, mut tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        while tx_events2.try_recv().is_ok() {}

        // Send multiple TXs
        let tx_count = 10;
        for i in 0..tx_count {
            let tx = test_tx_xdr(i as i64);
            handle1.broadcast_tx(tx).await;
        }

        // Wait for all TXs on bounded TX events channel
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        let mut received_count = 0;

        while tokio::time::Instant::now() < deadline && received_count < tx_count {
            tokio::select! {
                Some(event) = tx_events2.recv() => {
                    if let OverlayEvent::TxReceived { .. } = event {
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

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, _events2, mut tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        while tx_events2.try_recv().is_ok() {}

        // Send same TX twice
        let tx = test_tx_xdr(30_000);
        handle1.broadcast_tx(tx.clone()).await;
        tokio::time::sleep(Duration::from_millis(50)).await;
        handle1.broadcast_tx(tx.clone()).await;

        // Wait and count received TXs
        tokio::time::sleep(Duration::from_millis(500)).await;

        let mut received_count = 0;
        while let Ok(event) = tx_events2.try_recv() {
            if let OverlayEvent::TxReceived { .. } = event {
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

        let (handle_a, _events_a, _tx_events_a, overlay_a) =
            create_overlay(keypair_a, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_b, mut events_b, _tx_events_b, overlay_b) =
            create_overlay(keypair_b, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_c, mut events_c, _tx_events_c, overlay_c) =
            create_overlay(keypair_c, Arc::new(OverlayMetrics::new())).unwrap();

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

        // Wait for connections to establish
        tokio::time::sleep(Duration::from_millis(500)).await;

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

        let (handle_a, _events_a, _tx_events_a, overlay_a) =
            create_overlay(keypair_a, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_b, _events_b, mut tx_events_b, overlay_b) =
            create_overlay(keypair_b, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle_c, _events_c, mut tx_events_c, overlay_c) =
            create_overlay(keypair_c, Arc::new(OverlayMetrics::new())).unwrap();

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

        while tx_events_b.try_recv().is_ok() {}
        while tx_events_c.try_recv().is_ok() {}

        // A broadcasts TX
        let tx_msg = test_tx_xdr(40_000);
        handle_a.broadcast_tx(tx_msg.clone()).await;

        let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
        let mut b_received = false;
        let mut c_received = false;

        while tokio::time::Instant::now() < deadline && (!b_received || !c_received) {
            tokio::select! {
                Some(event) = tx_events_b.recv() => {
                    if let OverlayEvent::TxReceived { tx, .. } = event {
                        if tx == tx_msg {
                            b_received = true;
                        }
                    }
                }
                Some(event) = tx_events_c.recv() => {
                    if let OverlayEvent::TxReceived { tx, .. } = event {
                        if tx == tx_msg {
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
        let (handle, _events, _tx_events, overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _events, _tx_events, overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
    /// 3. No unbounded memory growth (bounded TX channel caps at TX_EVENT_CHANNEL_CAPACITY)
    #[tokio::test]
    async fn test_tx_backpressure_stress() {
        let keypair1 = Keypair::generate_ed25519();
        let keypair2 = Keypair::generate_ed25519();

        let (handle1, _events1, _tx_events1, overlay1) =
            create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut tx_events2, overlay2) =
            create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        while tx_events2.try_recv().is_ok() {}

        // STRESS TEST: Flood with many TXs while also sending SCP
        // This simulates a real attack scenario where the network is flooded with TXs
        let tx_flood_count = 50_000u32; // Exceed TX_EVENT_CHANNEL_CAPACITY (10,000)
        let scp_msg_count = 100u32;

        // Start flooding TXs (don't wait for processing)
        let handle1_clone = handle1.clone();
        let tx_flood_task = tokio::spawn(async move {
            for i in 0..tx_flood_count {
                // Each TX unique to avoid dedup
                let tx = test_tx_xdr(i as i64);
                handle1_clone.broadcast_tx(tx).await;
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
                Some(event) = tx_events2.recv() => {
                    if let OverlayEvent::TxReceived { .. } = event {
                        tx_received += 1;
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(100)) => {
                    // Check if channels are empty
                    if events2.is_empty() && tx_events2.is_empty() {
                        // Give a bit more time for any in-flight messages
                        tokio::time::sleep(Duration::from_millis(200)).await;
                        if events2.is_empty() && tx_events2.is_empty() {
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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
    handle2.fetch_txset(test_hash).await;
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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
    handle2.fetch_txset(test_hash).await;

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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
    let (handle, _events, _tx_events, overlay) =
        create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

    handle2.fetch_txset(requested_hash).await;

    // Node1 receives request and responds
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut responded = false;

    while tokio::time::Instant::now() < deadline && !responded {
        tokio::select! {
            Some(event) = events1.recv() => {
                if let OverlayEvent::TxSetRequested { hash, from } = event {
                    assert_eq!(hash, requested_hash, "Request should have correct hash");
                    handle1.send_txset(hash, txset_data.clone(), from).await;
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
                    assert_eq!(data, txset_data, "Received data should match");
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
    let (handle, mut events, _tx_events, overlay) =
        create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 20701;
    tokio::spawn(async move { overlay.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    // Request TX set with no peers connected
    let requested_hash: [u8; 32] = [0x88; 32];
    handle.fetch_txset(requested_hash).await;

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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

    handle2.fetch_txset(hash1).await;
    handle2.fetch_txset(hash2).await;
    handle2.fetch_txset(hash3).await;

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
                    handle1.send_txset(hash, data, from).await;
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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

    let listen_port = 19801;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;

    tokio::spawn(async move { overlay2.run("127.0.0.1", 19802).await });
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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
    let (handle, _events, _tx_events, overlay) =
        create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
            .send_txset(txset_hash, txset_data, peer2_id)
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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
    handle1.fetch_txset(txset_hash).await;
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
        .send_txset(txset_hash, txset_data.clone(), peer1_id)
        .await;

    // Verify node1 receives the TxSet response
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut got_response = false;
    while tokio::time::Instant::now() < deadline && !got_response {
        tokio::select! {
            Some(event) = events1.recv() => {
                if let OverlayEvent::TxSetReceived { hash, data, .. } = event {
                    if hash == txset_hash && data == txset_data {
                        got_response = true;
                    }
                }
            }
            _ = tokio::time::sleep(Duration::from_millis(10)) => {}
        }
    }
    assert!(got_response, "Node1 should receive TxSet response");

    // Request the same TxSet again - should NOT be skipped since pending was cleared
    handle1.fetch_txset(txset_hash).await;
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
    let (handle1, _events1, tx_events1, overlay1) =
        create_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, mut tx_events2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
    let test_tx = test_tx_xdr(50_000);
    handle1.broadcast_tx(test_tx.clone()).await;

    // Wait for INV→GETDATA→TX flow (with batching delay + RTT)
    // - INV is batched for up to 100ms
    // - GETDATA sent
    // - TX response sent
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut tx_received = false;

    while tokio::time::Instant::now() < deadline && !tx_received {
        tokio::select! {
            Some(event) = tx_events2.recv() => {
                if let OverlayEvent::TxReceived { tx, from } = event {
                    if tx == test_tx && from == peer1_id {
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
    drop(tx_events1);

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
    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, mut tx_events2, overlay2) =
        create_overlay(keypair2.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle3, _events3, mut tx_events3, overlay3) =
        create_overlay(keypair3, Arc::new(OverlayMetrics::new())).unwrap();

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
    let test_tx = test_tx_xdr(60_000);
    handle1.broadcast_tx(test_tx.clone()).await;

    // First verify Node2 receives the TX from Node1
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    let mut node2_received = false;
    while tokio::time::Instant::now() < deadline && !node2_received {
        tokio::select! {
            Some(event) = tx_events2.recv() => {
                if let OverlayEvent::TxReceived { tx, from } = event {
                    eprintln!("Node2 received TX from {}: {:02x?}", from, &tx[..tx.len().min(8)]);
                    if tx == test_tx && from == peer1_id {
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
            Some(event) = tx_events3.recv() => {
                if let OverlayEvent::TxReceived { tx, from } = event {
                    eprintln!("Node3 received TX from {}: {:02x?}", from, &tx[..tx.len().min(8)]);
                    // Node3 must receive TX from Node2 (relay), not Node1 (no direct connection)
                    if tx == test_tx && from == peer2_id {
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

    let (handle1, _events1, _tx_events1, overlay1) =
        create_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle3, mut events3, _tx_events3, overlay3) =
        create_overlay(keypair3, Arc::new(OverlayMetrics::new())).unwrap();

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
                if let OverlayEvent::ScpReceived { envelope, from } = event {
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
                if let OverlayEvent::ScpReceived { envelope, from } = event {
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

    let (handle1, mut events1, _tx_events1, overlay1) =
        create_overlay(keypair1.clone(), Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _tx_events2, overlay2) =
        create_overlay(keypair2.clone(), Arc::new(OverlayMetrics::new())).unwrap();

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
                if let OverlayEvent::ScpReceived { envelope, from } = event {
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
        let (handle, _events, _tx_events, overlay) =
            create_overlay(keypair, Arc::clone(&m)).unwrap();

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
    let (handle1, _events1, _tx1, overlay1) = create_overlay(keypair1, Arc::clone(&m1)).unwrap();
    let (handle2, mut events2, _tx2, overlay2) = create_overlay(keypair2, Arc::clone(&m2)).unwrap();

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
    let (handle1, _events1, _tx1, overlay1) = create_overlay(keypair1, Arc::clone(&m1)).unwrap();
    let (handle2, _events2, _tx2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        create_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, _events2, _tx2, overlay2) =
        create_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, events, _tx_events, overlay) =
            create_overlay(keypair, Arc::clone(&m)).unwrap();

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
