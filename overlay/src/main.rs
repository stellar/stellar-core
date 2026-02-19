//! Stellar Overlay Process
//!
//! A process-isolated overlay for stellar-core that handles:
//! - SCP message relay (latency-critical, via dedicated QUIC stream)
//! - Transaction flooding (via dedicated QUIC stream)
//! - Peer management
//!
//! Uses QUIC transport for true stream independence - SCP never blocked by TX.
//! Communicates with Core via Unix domain socket IPC.

mod config;
mod flood;
mod http;
pub mod integrated;
mod ipc;
pub mod libp2p_overlay;

use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use tokio::sync::{broadcast, mpsc, RwLock};
use tracing::{debug, error, info, warn};

use config::Config;
use flood::{build_tx_set_xdr, hash_tx_set, CachedTxSet, Hash256, TxSetCache};
use integrated::{CoreCommand, Overlay, OverlayHandle};
use ipc::{CoreIpc, Message, MessageType};
use libp2p::identity::Keypair as Libp2pKeypair;
use libp2p::PeerId;
use libp2p_overlay::{
    create_overlay, OverlayEvent as LibP2pOverlayEvent, OverlayHandle as LibP2pOverlayHandle,
    StellarOverlay,
};

/// Command-line arguments
struct Args {
    config_path: Option<PathBuf>,
    socket_path: Option<PathBuf>,
    listen_mode: bool,
    peer_port: Option<u16>,
}

impl Args {
    fn parse() -> Self {
        let mut args = Args {
            config_path: None,
            socket_path: None,
            listen_mode: false,
            peer_port: None,
        };

        let mut iter = std::env::args().skip(1);
        while let Some(arg) = iter.next() {
            match arg.as_str() {
                "--config" | "-c" => {
                    args.config_path = iter.next().map(PathBuf::from);
                }
                "--socket" | "-s" => {
                    args.socket_path = iter.next().map(PathBuf::from);
                }
                "--peer-port" | "-p" => {
                    args.peer_port = iter.next().and_then(|s| s.parse().ok());
                }
                "--listen" | "-l" => {
                    args.listen_mode = true;
                    // Check if next arg is the socket path (C++ passes it this way)
                    if let Some(next) = iter.next() {
                        if !next.starts_with('-') {
                            args.socket_path = Some(PathBuf::from(next));
                        }
                    }
                }
                "--help" | "-h" => {
                    eprintln!("Usage: stellar-overlay [OPTIONS] [SOCKET_PATH]");
                    eprintln!();
                    eprintln!("Options:");
                    eprintln!("  -c, --config <PATH>    Path to config file (TOML)");
                    eprintln!("  -s, --socket <PATH>    Path to Core IPC socket");
                    eprintln!("  -p, --peer-port <PORT> Port for peer TCP connections");
                    eprintln!(
                        "  -l, --listen           Listen mode (create socket, wait for Core)"
                    );
                    eprintln!("  -h, --help             Show this help");
                    eprintln!();
                    eprintln!("By default, connects to an existing socket. Use --listen to create");
                    eprintln!("the socket and wait for Core to connect (useful for testing).");
                    std::process::exit(0);
                }
                other => {
                    // Positional arg - treat as socket path for backward compat
                    if args.socket_path.is_none() {
                        args.socket_path = Some(PathBuf::from(other));
                    }
                }
            }
        }

        args
    }
}

/// Extract TX set hashes from an SCP envelope (best effort, may return empty)
/// The SCP envelope contains StellarValue(s) which start with a 32-byte txSetHash.
/// We look for these hashes without fully parsing the XDR - just scanning for them.
fn extract_txset_hashes_from_scp(envelope: &[u8]) -> Vec<[u8; 32]> {
    // StellarValue structure:
    //   Hash txSetHash;      // 32 bytes
    //   TimePoint closeTime; // uint64 (8 bytes)
    //   UpgradeType upgrades<6>;
    //   union switch (StellarValueType v) { ... }
    //
    // The txSetHash appears in SCPBallot.value within various statement types.
    // Rather than fully parsing, we look for 32-byte sequences that could be hashes.
    // This is imperfect but catches most cases.
    //
    // SCP statement types that contain StellarValue:
    // - PREPARE: ballot.value, prepared.value, preparedPrime.value
    // - CONFIRM: ballot.value
    // - EXTERNALIZE: commit.value
    // - NOMINATE: votes<>, accepted<>

    let mut hashes = Vec::new();

    // Skip the nodeID (32 bytes) and slotIndex (8 bytes) at the start of SCPStatement
    // Then we have the pledges union...
    // This is too complex to parse reliably without proper XDR decoding.

    // Simple heuristic: look for 32-byte sequences followed by a reasonable timestamp
    // (timestamps are 8-byte uint64s, Stellar timestamps are ~1.7B for year 2024)
    if envelope.len() < 48 {
        return hashes;
    }

    // Scan through looking for potential StellarValue structures
    for i in 0..envelope.len().saturating_sub(40) {
        // Check if bytes [i..i+32] could be a hash followed by a valid timestamp
        if i + 40 <= envelope.len() {
            let potential_timestamp =
                u64::from_be_bytes(envelope[i + 32..i + 40].try_into().unwrap_or([0; 8]));
            // Stellar timestamps are Unix time, valid range ~1.5B to ~2B for 2020-2033
            if potential_timestamp > 1_500_000_000 && potential_timestamp < 2_500_000_000 {
                let mut hash = [0u8; 32];
                hash.copy_from_slice(&envelope[i..i + 32]);
                // Avoid duplicates
                if !hashes.contains(&hash) {
                    hashes.push(hash);
                }
            }
        }
    }

    hashes
}

/// Application state
struct App {
    #[allow(dead_code)]
    config: Config,
    core_ipc: CoreIpc,
    overlay_handle: OverlayHandle,
    /// Cache for built TX sets
    tx_set_cache: Arc<RwLock<TxSetCache>>,
    /// TX set hashes already pushed to Core (reset on ledger close)
    pushed_tx_sets: Arc<RwLock<HashSet<Hash256>>>,
    /// Current ledger sequence
    current_ledger_seq: Arc<RwLock<u32>>,
    /// libp2p overlay handle (QUIC-based SCP + TX)
    libp2p_handle: LibP2pOverlayHandle,
    /// libp2p overlay events (SCP, TxSet - critical, unbounded)
    libp2p_events: mpsc::UnboundedReceiver<LibP2pOverlayEvent>,
    /// libp2p TX events (bounded, may drop under backpressure)
    tx_events: mpsc::Receiver<LibP2pOverlayEvent>,
    /// TX sets that Core has requested but we're still fetching from peers
    pending_core_txset_requests: Arc<RwLock<HashSet<Hash256>>>,
    /// Pending SCP state requests: maps request_id to requesting peer
    /// When Core responds with ScpStateResponse containing request_id, we look up the peer
    pending_scp_state_requests: Arc<RwLock<HashMap<u64, PeerId>>>,
    /// Counter for generating unique SCP state request IDs
    next_scp_request_id: Arc<AtomicU64>,
}

impl App {
    async fn new(config: Config, listen_mode: bool) -> Result<Self, Box<dyn std::error::Error>> {
        // Connect to Core (or listen for connection)
        let core_ipc = if listen_mode {
            CoreIpc::listen(&config.core_socket).await?
        } else {
            CoreIpc::connect(&config.core_socket).await?
        };

        // Create channels for mempool manager communication
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();

        // Create mempool manager (no network - libp2p handles all P2P)
        let mempool_manager = Overlay::new(cmd_rx);
        let overlay_handle = OverlayHandle::new(cmd_tx);

        // Spawn mempool manager task
        tokio::spawn(async move {
            if let Err(e) = mempool_manager.run().await {
                error!("Mempool manager error: {}", e);
            }
        });

        // Create libp2p QUIC overlay for SCP + TX + TxSet (unified, independent streams)
        let libp2p_keypair = Libp2pKeypair::generate_ed25519();
        let (libp2p_handle, libp2p_event_rx, tx_event_rx, libp2p_overlay) =
            create_overlay(libp2p_keypair)
                .map_err(|e| format!("Failed to create libp2p overlay: {}", e))?;

        // Use peer_port + 1000 for libp2p QUIC to avoid collision with legacy TCP
        let libp2p_port = config.peer_port + 1000;
        let libp2p_listen_ip = config.libp2p_listen_ip.clone();

        // Spawn libp2p overlay task
        tokio::spawn(async move {
            libp2p_overlay.run(&libp2p_listen_ip, libp2p_port).await;
        });

        info!(
            "Started libp2p QUIC overlay on {}:{} (SCP + TX + TxSet streams)",
            config.libp2p_listen_ip, libp2p_port
        );

        Ok(Self {
            config,
            core_ipc,
            overlay_handle,
            tx_set_cache: Arc::new(RwLock::new(TxSetCache::new(100))),
            pushed_tx_sets: Arc::new(RwLock::new(HashSet::new())),
            current_ledger_seq: Arc::new(RwLock::new(0)),
            libp2p_handle,
            libp2p_events: libp2p_event_rx,
            tx_events: tx_event_rx,
            pending_core_txset_requests: Arc::new(RwLock::new(HashSet::new())),
            pending_scp_state_requests: Arc::new(RwLock::new(HashMap::new())),
            next_scp_request_id: Arc::new(AtomicU64::new(1)),
        })
    }

    /// Main event loop - process messages from Core and overlay events
    async fn run(mut self) {
        info!("Overlay started, processing Core messages");

        loop {
            tokio::select! {
                // Receive message from Core
                msg = self.core_ipc.receiver.recv() => {
                    match msg {
                        Some(msg) => self.handle_core_message(msg).await,
                        None => {
                            info!("Core IPC connection closed");
                            break;
                        }
                    }
                }

                // Receive events from libp2p QUIC overlay (SCP + TxSet - critical)
                Some(event) = self.libp2p_events.recv() => {
                    self.handle_libp2p_event(event).await;
                }

                // Receive TX events from libp2p (bounded channel, may drop under backpressure)
                Some(event) = self.tx_events.recv() => {
                    self.handle_libp2p_event(event).await;
                }
            }
        }

        // Shutdown libp2p
        self.libp2p_handle.shutdown().await;

        info!("Overlay shutting down");
    }

    /// Handle an event from the libp2p QUIC overlay (SCP + TX)
    async fn handle_libp2p_event(&mut self, event: LibP2pOverlayEvent) {
        match event {
            LibP2pOverlayEvent::ScpReceived { envelope, from } => {
                // Copy first 4 bytes for logging identification
                let mut id_bytes = [0u8; 4];
                let id_len = std::cmp::min(envelope.len(), 4);
                id_bytes[..id_len].copy_from_slice(&envelope[..id_len]);

                info!(
                    "SCP_FROM_PEER: Received SCP (id={:02x?}) ({} bytes) from {}, forwarding to Core",
                    &id_bytes[..id_len],
                    envelope.len(),
                    from
                );

                // Extract TX set hashes and proactively fetch them
                let txset_hashes = extract_txset_hashes_from_scp(&envelope);
                for txhash in &txset_hashes {
                    // Record peer as source for this TX set
                    debug!(
                        "Recording peer {} as source for TX set {:02x?}...",
                        from,
                        &txhash[..4]
                    );
                    self.libp2p_handle.record_txset_source(*txhash, from).await;

                    // Proactively fetch if not already cached
                    // (libp2p layer deduplicates if fetch already in progress)
                    let is_cached = {
                        let cache = self.tx_set_cache.read().await;
                        cache.get(txhash).is_some()
                    };
                    if !is_cached {
                        info!(
                            "TXSET_AUTO_FETCH: Proactively fetching TX set {:02x?}... referenced in SCP from {}",
                            &txhash[..4],
                            from
                        );
                        self.libp2p_handle.fetch_txset(*txhash).await;
                    }
                }

                // Forward to Core
                if let Err(e) = self.core_ipc.sender.send_scp_received(envelope, 0) {
                    error!(
                        "SCP_TO_CORE_FAIL: Failed to send SCP (id={:02x?}) to Core: {}",
                        &id_bytes[..id_len],
                        e
                    );
                } else {
                    debug!(
                        "SCP_TO_CORE_OK: Forwarded SCP (id={:02x?}) to Core",
                        &id_bytes[..id_len]
                    );
                }
            }
            LibP2pOverlayEvent::TxReceived { tx, from } => {
                debug!("Received TX via QUIC from {}: {} bytes", from, tx.len());
                // Add to mempool
                // TODO: Parse XDR to extract fee and ops instead of hardcoding fee=0, ops=1
                // This causes network-flooded TXs to have wrong priority in mempool
                // and breaks fee-based eviction. Need to:
                // 1. Parse TransactionEnvelope XDR to get tx.fee and operation count
                // 2. Extract source account and sequence number
                // 3. Consider signature validation to prevent spam
                self.overlay_handle.submit_tx(tx, 0, 1);
            }
            LibP2pOverlayEvent::TxSetReceived { hash, data, from } => {
                info!(
                    "TXSET_RECV: Received TxSet {:02x?}... ({} bytes) from {}",
                    &hash[..4],
                    data.len(),
                    from
                );

                // IMPORTANT: Cache the TxSet FIRST, before pushing to Core
                // This ensures the TxSet is available when SCP processing resumes
                // and Core subsequently broadcasts the SCP to other peers
                {
                    let mut cache = self.tx_set_cache.write().await;
                    cache.insert(CachedTxSet {
                        hash,
                        xdr: data.clone(),
                        ledger_seq: 0,
                        tx_hashes: vec![],
                    });
                }

                // Always push TX set to Core (Core handles dedup)
                info!(
                    "TXSET_TO_CORE: Pushing TxSet {:02x?}... ({} bytes) to Core",
                    &hash[..4],
                    data.len()
                );
                if let Err(e) = self
                    .core_ipc
                    .sender
                    .send_tx_set_available(hash, data.clone())
                {
                    error!("Failed to push TX set to Core: {}", e);
                }
            }
            LibP2pOverlayEvent::TxSetRequested { hash, from } => {
                info!("Peer {} requesting TxSet {:02x?}...", from, &hash[..4]);
                // Look up in local cache and respond
                let cache = self.tx_set_cache.read().await;
                if let Some(cached) = cache.get(&hash) {
                    info!(
                        "Serving TxSet {:02x?}... ({} bytes) to {}",
                        &hash[..4],
                        cached.xdr.len(),
                        from
                    );
                    let handle = self.libp2p_handle.clone();
                    let data = cached.xdr.clone();
                    tokio::spawn(async move {
                        handle.send_txset(hash, data, from).await;
                    });
                } else {
                    warn!(
                        "TxSet {:02x?}... NOT IN CACHE - cannot serve to {} (cache has {} entries)",
                        &hash[..4],
                        from,
                        cache.len()
                    );
                }
            }

            LibP2pOverlayEvent::ScpStateRequested {
                peer_id,
                ledger_seq,
            } => {
                // Generate unique request ID
                let request_id = self.next_scp_request_id.fetch_add(1, Ordering::SeqCst);
                info!(
                    "Peer {} requesting SCP state for ledger >= {} (request_id={})",
                    peer_id, ledger_seq, request_id
                );

                // Store mapping from request_id to peer_id
                self.pending_scp_state_requests
                    .write()
                    .await
                    .insert(request_id, peer_id);

                // Request SCP state from Core with request_id and ledger_seq
                // Payload format: [request_id:8][ledger_seq:4]
                let mut payload = Vec::with_capacity(12);
                payload.extend_from_slice(&request_id.to_le_bytes());
                payload.extend_from_slice(&ledger_seq.to_le_bytes());
                let msg = Message::new(MessageType::PeerRequestsScpState, payload);
                if let Err(e) = self.core_ipc.sender.send(msg) {
                    error!("Failed to send PeerRequestsScpState to Core: {:?}", e);
                    // Remove from map on error
                    self.pending_scp_state_requests
                        .write()
                        .await
                        .remove(&request_id);
                }
            }

            LibP2pOverlayEvent::PeerDisconnected { peer_id } => {
                // Clean up any pending SCP state requests for this peer
                let mut pending = self.pending_scp_state_requests.write().await;
                let before_len = pending.len();
                pending.retain(|_request_id, p| p != &peer_id);
                let removed = before_len - pending.len();
                if removed > 0 {
                    debug!(
                        "Removed {} pending SCP state requests for disconnected peer {}",
                        removed, peer_id
                    );
                }
            }
        }
    }

    /// Handle a message from Core
    async fn handle_core_message(&mut self, msg: Message) {
        match msg.msg_type {
            MessageType::Shutdown => {
                info!("Shutdown requested by Core");
                // Exit the process
                std::process::exit(0);
            }

            MessageType::BroadcastScp => {
                // Forward SCP broadcast via libp2p QUIC (dedicated stream, no blocking)
                let id_bytes = if msg.payload.len() >= 4 {
                    &msg.payload[..4]
                } else {
                    &msg.payload[..]
                };

                info!(
                    "SCP_FROM_CORE: Core requested broadcast of SCP (id={:02x?}) ({} bytes)",
                    id_bytes,
                    msg.payload.len()
                );
                let handle = self.libp2p_handle.clone();
                let payload = msg.payload;
                tokio::spawn(async move {
                    handle.broadcast_scp(payload).await;
                });
            }

            MessageType::GetTopTxs => {
                // Parse payload: [count:4]
                if msg.payload.len() < 4 {
                    warn!("GetTopTxs payload too short: {} bytes", msg.payload.len());
                    // Send empty response
                    if let Err(e) = self.core_ipc.sender.send_top_txs_response(&[]) {
                        error!("Failed to send empty top txs response: {}", e);
                    }
                    return;
                }

                let count = u32::from_le_bytes(msg.payload[0..4].try_into().unwrap()) as usize;
                info!("Core requesting top {} transactions", count);

                let core_sender = self.core_ipc.sender.clone();
                let overlay_handle = self.overlay_handle.clone();

                tokio::spawn(async move {
                    let txs = match tokio::time::timeout(
                        std::time::Duration::from_millis(100),
                        overlay_handle.get_top_txs(count),
                    )
                    .await
                    {
                        Ok(txs) => txs,
                        Err(_) => {
                            warn!("Timeout getting transactions from mempool");
                            vec![]
                        }
                    };

                    info!("Returning {} transactions to Core", txs.len());

                    // Extract just the TX data (not hashes) for the response
                    let tx_data: Vec<&[u8]> = txs.iter().map(|(_, d)| d.as_slice()).collect();

                    if let Err(e) = core_sender.send_top_txs_response(&tx_data) {
                        error!("Failed to send top txs response: {}", e);
                    }
                });
            }

            MessageType::RequestTxSet => {
                // Request TX set by hash - check local cache first, then fetch from peers via libp2p
                if msg.payload.len() < 32 {
                    warn!("RequestTxSet payload too short");
                    return;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);

                let tx_set_cache = Arc::clone(&self.tx_set_cache);
                let core_sender = self.core_ipc.sender.clone();
                let libp2p_handle = self.libp2p_handle.clone();
                let pending_requests = Arc::clone(&self.pending_core_txset_requests);

                tokio::spawn(async move {
                    // First check local cache
                    {
                        let cache = tx_set_cache.read().await;
                        if let Some(cached) = cache.get(&hash) {
                            info!(
                                "TXSET_FROM_CACHE: Sending TX set {:02x?}... ({} bytes) from local cache",
                                &hash[..4],
                                cached.xdr.len()
                            );
                            if let Err(e) =
                                core_sender.send_tx_set_available(hash, cached.xdr.clone())
                            {
                                error!("Failed to send TX set: {}", e);
                            }
                            return;
                        }
                    }

                    // Not in local cache - mark as pending and request from peers
                    info!(
                        "TXSET_FETCH_START: TX set {:02x?}... not in cache, fetching from peers",
                        &hash[..4]
                    );
                    pending_requests.write().await.insert(hash);
                    libp2p_handle.fetch_txset(hash).await;
                });
            }

            MessageType::CacheTxSet => {
                // Core built a TX set locally and wants us to cache it for peer requests
                // Payload: [hash:32][txSetXDR...]
                if msg.payload.len() < 33 {
                    warn!("CacheTxSet payload too short");
                    return;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);
                let xdr = msg.payload[32..].to_vec();

                info!(
                    "TXSET_CACHE: Caching locally-built TX set {:02x?}... ({} bytes)",
                    &hash[..4],
                    xdr.len()
                );

                let mut cache = self.tx_set_cache.write().await;
                cache.insert(CachedTxSet {
                    hash,
                    xdr,
                    ledger_seq: 0,
                    tx_hashes: vec![],
                });
            }

            MessageType::SubmitTx => {
                // Parse payload: [fee:i64][numOps:u32][txEnvelope...]
                if msg.payload.len() < 12 {
                    warn!("SubmitTx payload too short");
                    return;
                }

                let fee = i64::from_le_bytes(msg.payload[0..8].try_into().unwrap());
                let num_ops = u32::from_le_bytes(msg.payload[8..12].try_into().unwrap());
                let tx_data = msg.payload[12..].to_vec();

                // Add to mempool
                self.overlay_handle
                    .submit_tx(tx_data.clone(), fee as u64, num_ops);

                // Broadcast TX via libp2p QUIC (dedicated stream)
                let handle = self.libp2p_handle.clone();
                tokio::spawn(async move {
                    handle.broadcast_tx(tx_data).await;
                });
            }

            MessageType::RequestScpState => {
                // Core is asking us to request SCP state from peers
                // Payload is ledger sequence (u32, 4 bytes)
                if msg.payload.len() >= 4 {
                    let ledger_seq = u32::from_le_bytes(msg.payload[0..4].try_into().unwrap());
                    info!(
                        "Core requests SCP state from peers for ledger >= {}",
                        ledger_seq
                    );

                    // Forward request to all connected peers
                    let handle = self.libp2p_handle.clone();
                    tokio::spawn(async move {
                        handle.request_scp_state_from_all_peers(ledger_seq).await;
                    });
                } else {
                    warn!(
                        "RequestScpState with invalid payload length: {}",
                        msg.payload.len()
                    );
                }
            }

            MessageType::LedgerClosed => {
                // Parse payload: [ledgerSeq:4][ledgerHash:32]
                if msg.payload.len() >= 4 {
                    let ledger_seq = u32::from_le_bytes(msg.payload[0..4].try_into().unwrap());
                    info!("Ledger {} closed", ledger_seq);

                    let current_seq = Arc::clone(&self.current_ledger_seq);
                    let pushed = Arc::clone(&self.pushed_tx_sets);
                    let cache = Arc::clone(&self.tx_set_cache);

                    tokio::spawn(async move {
                        // Update current ledger
                        *current_seq.write().await = ledger_seq;

                        // Clear pushed TX sets (reset dedup tracking)
                        pushed.write().await.clear();

                        // Evict old TX sets from cache
                        cache
                            .write()
                            .await
                            .evict_before(ledger_seq.saturating_sub(5));
                    });
                }
            }

            MessageType::TxSetExternalized => {
                // Parse payload: [txSetHash:32][numTxHashes:4][txHash1:32][txHash2:32]...
                if msg.payload.len() >= 36 {
                    let mut tx_set_hash = [0u8; 32];
                    tx_set_hash.copy_from_slice(&msg.payload[0..32]);
                    let num_hashes =
                        u32::from_le_bytes(msg.payload[32..36].try_into().unwrap()) as usize;

                    info!(
                        "TX set externalized: {:?} with {} TX hashes",
                        &tx_set_hash[..4],
                        num_hashes
                    );

                    // Parse TX hashes from payload
                    let mut tx_hashes = Vec::with_capacity(num_hashes);
                    for i in 0..num_hashes {
                        let start = 36 + (i * 32);
                        let end = start + 32;
                        if end <= msg.payload.len() {
                            let mut hash = [0u8; 32];
                            hash.copy_from_slice(&msg.payload[start..end]);
                            tx_hashes.push(hash);
                        }
                    }

                    // Remove TXs from mempool and WAIT for completion
                    // This prevents race where next nomination queries stale mempool
                    if !tx_hashes.is_empty() {
                        let overlay_handle = self.overlay_handle.clone();
                        // Spawn but await the task to ensure completion before returning
                        let task = tokio::spawn(async move {
                            overlay_handle.remove_txs_sync(tx_hashes).await;
                        });
                        let _ = task.await;
                    }

                    // NOTE: Don't remove TX set from cache on externalization!
                    // Other nodes may still need to fetch it for catch-up.
                    // The evict_before() call in LedgerClosed handler will clean
                    // up old TX sets (keeping last 5 ledgers).
                }
            }

            MessageType::ScpStateResponse => {
                // Core responded with SCP state - look up peer by request_id and forward
                // Payload format: [request_id:8][count:4][env1_len:4][env1_xdr]...
                if msg.payload.len() < 12 {
                    warn!(
                        "ScpStateResponse payload too short: {} (need at least 12 bytes)",
                        msg.payload.len()
                    );
                    return;
                }

                let request_id = u64::from_le_bytes(msg.payload[0..8].try_into().unwrap());
                let num_envelopes =
                    u32::from_le_bytes(msg.payload[8..12].try_into().unwrap()) as usize;
                info!(
                    "Core responded with {} SCP envelopes for request_id={}",
                    num_envelopes, request_id
                );

                // Look up the peer by request_id
                let peer_id = {
                    let mut pending = self.pending_scp_state_requests.write().await;
                    match pending.remove(&request_id) {
                        Some(p) => p,
                        None => {
                            warn!(
                                "Received ScpStateResponse for unknown request_id={} - dropping",
                                request_id
                            );
                            return;
                        }
                    }
                };

                info!(
                    "Forwarding {} SCP envelopes to peer {} (request_id={})",
                    num_envelopes, peer_id, request_id
                );

                // Parse and forward each envelope to the requesting peer
                let handle = self.libp2p_handle.clone();
                let payload = msg.payload.clone();
                tokio::spawn(async move {
                    let mut offset = 12; // Skip request_id (8) + count (4)
                    for _ in 0..num_envelopes {
                        if offset + 4 > payload.len() {
                            warn!("ScpStateResponse truncated at envelope length");
                            break;
                        }
                        let env_len =
                            u32::from_le_bytes(payload[offset..offset + 4].try_into().unwrap())
                                as usize;
                        offset += 4;

                        if offset + env_len > payload.len() {
                            warn!("ScpStateResponse truncated at envelope data");
                            break;
                        }
                        let envelope = &payload[offset..offset + env_len];
                        offset += env_len;

                        // Send envelope to requesting peer over SCP stream
                        if let Err(e) = handle.send_scp_to_peer(peer_id.clone(), envelope).await {
                            warn!("Failed to send SCP envelope to {}: {:?}", peer_id, e);
                        }
                    }
                    info!(
                        "Finished forwarding {} SCP envelopes to {}",
                        num_envelopes, peer_id
                    );
                });
            }

            MessageType::SetPeerConfig => {
                // Parse JSON payload and configure peer connections
                if let Ok(json_str) = std::str::from_utf8(&msg.payload) {
                    info!("Received peer config: {}", json_str);
                    if let Ok(config) = serde_json::from_str::<serde_json::Value>(json_str) {
                        let known: Vec<String> = config["known_peers"]
                            .as_array()
                            .map(|v| {
                                v.iter()
                                    .filter_map(|s| s.as_str().map(String::from))
                                    .collect()
                            })
                            .unwrap_or_default();
                        let preferred: Vec<String> = config["preferred_peers"]
                            .as_array()
                            .map(|v| {
                                v.iter()
                                    .filter_map(|s| s.as_str().map(String::from))
                                    .collect()
                            })
                            .unwrap_or_default();
                        let listen_port = config["listen_port"].as_u64().unwrap_or(11625) as u16;

                        info!(
                            "Parsed peer config: known={:?}, preferred={:?}, port={}",
                            known, preferred, listen_port
                        );

                        // Connect libp2p QUIC to all known/preferred peers
                        let all_peers: Vec<_> =
                            known.into_iter().chain(preferred.into_iter()).collect();
                        let peer_count = all_peers.len();

                        for addr_str in all_peers {
                            if let Ok(addr) = addr_str.parse::<SocketAddr>() {
                                // QUIC uses UDP, port + 1000
                                let libp2p_port = addr.port() + 1000;
                                let libp2p_addr: libp2p::Multiaddr =
                                    format!("/ip4/{}/udp/{}/quic-v1", addr.ip(), libp2p_port)
                                        .parse()
                                        .unwrap();

                                let handle = self.libp2p_handle.clone();
                                tokio::spawn(async move {
                                    handle.dial(libp2p_addr).await;
                                });
                            }
                        }

                        // Kademlia bootstrap is now triggered automatically when the first peer
                        // is identified (in handle_identify_event). This ensures the routing
                        // table has at least one peer before attempting bootstrap, avoiding
                        // "No known peers" errors in slow network environments like k8s.

                        // TODO: Consider adding periodic re-bootstrap for network maintenance
                        // Kademlia routing tables become stale as peers join/leave
                        // Re-bootstrapping every 5-10 minutes keeps routing fresh
                    }
                }
            }

            _ => {
                warn!("Unexpected message type from Core: {:?}", msg.msg_type);
            }
        }
    }
}

fn setup_logging(level: &str) {
    use tracing_subscriber::{fmt, EnvFilter};

    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new(level));

    fmt()
        .with_env_filter(filter)
        .with_target(true)
        .with_thread_ids(false)
        .with_file(false)
        .with_line_number(false)
        .init();
}

#[tokio::main]
async fn main() {
    // Install panic hook to log panics properly
    std::panic::set_hook(Box::new(|panic_info| {
        eprintln!("PANIC in Rust overlay: {}", panic_info);
        if let Some(location) = panic_info.location() {
            eprintln!(
                "  at {}:{}:{}",
                location.file(),
                location.line(),
                location.column()
            );
        }
        if let Some(s) = panic_info.payload().downcast_ref::<&str>() {
            eprintln!("  payload: {}", s);
        } else if let Some(s) = panic_info.payload().downcast_ref::<String>() {
            eprintln!("  payload: {}", s);
        }
    }));

    let args = Args::parse();

    // Load config
    let mut config = if let Some(path) = &args.config_path {
        match Config::from_file(path) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("Failed to load config: {}", e);
                std::process::exit(1);
            }
        }
    } else {
        Config::default()
    };

    // Override socket path from command line
    if let Some(socket) = args.socket_path {
        config.core_socket = socket;
    }

    // Override peer port from command line
    if let Some(port) = args.peer_port {
        config.peer_port = port;
    }

    // Validate config
    if let Err(e) = config.validate() {
        eprintln!("Invalid config: {}", e);
        std::process::exit(1);
    }

    // Setup logging
    setup_logging(&config.log_level);

    info!("Stellar Overlay starting");
    info!("Core socket: {}", config.core_socket.display());
    info!("Peer port: {}", config.peer_port);
    info!(
        "Mode: {}",
        if args.listen_mode {
            "listen (server)"
        } else {
            "connect (client)"
        }
    );

    // Handle SIGTERM/SIGINT for graceful shutdown
    let shutdown = async {
        let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("Failed to register SIGTERM handler");

        let mut sigint = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())
            .expect("Failed to register SIGINT handler");

        tokio::select! {
            _ = sigterm.recv() => info!("Received SIGTERM"),
            _ = sigint.recv() => info!("Received SIGINT"),
        }
    };

    // Create and run app
    let app = match App::new(config, args.listen_mode).await {
        Ok(app) => app,
        Err(e) => {
            error!("Failed to initialize overlay: {}", e);
            std::process::exit(1);
        }
    };

    // Run until shutdown signal or Core disconnects
    tokio::select! {
        _ = app.run() => {}
        _ = shutdown => {
            info!("Shutdown signal received");
        }
    }

    info!("Overlay stopped");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_extract_txset_hashes_empty() {
        // Empty envelope should return no hashes
        assert!(extract_txset_hashes_from_scp(&[]).is_empty());

        // Short envelope should return no hashes
        assert!(extract_txset_hashes_from_scp(&[0u8; 40]).is_empty());
    }

    #[test]
    fn test_extract_txset_hashes_with_valid_timestamp() {
        // Create a mock envelope with a known hash followed by a valid timestamp
        let mut envelope = vec![0u8; 100];

        // Place a known hash at offset 10
        let expected_hash: [u8; 32] = [
            0x88, 0x71, 0x32, 0x79, 0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
            0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x12, 0x34, 0x56, 0x78,
            0x9A, 0xBC, 0xDE, 0xF0,
        ];
        envelope[10..42].copy_from_slice(&expected_hash);

        // Place a valid timestamp (2024 = ~1704067200 = 0x65944600) after the hash
        // XDR uses big-endian, timestamp ~1.7B = 0x00000000_65944600
        let timestamp: u64 = 1704067200; // Jan 1, 2024
        envelope[42..50].copy_from_slice(&timestamp.to_be_bytes());

        let hashes = extract_txset_hashes_from_scp(&envelope);

        assert_eq!(hashes.len(), 1, "Should find exactly one hash");
        assert_eq!(hashes[0], expected_hash, "Should match expected hash");
    }

    #[test]
    fn test_extract_txset_hashes_invalid_timestamp() {
        // Create envelope with hash followed by invalid timestamp (too old)
        // Use 0x00 fill with specific placement to avoid accidental valid timestamps
        // The heuristic scanner can find false positives, so we construct carefully
        let mut envelope = vec![0x00u8; 50]; // Minimal size, all zeros

        let hash: [u8; 32] = [0x42u8; 32];
        envelope[0..32].copy_from_slice(&hash);

        // Invalid timestamp (year 1970) at offset 32
        let bad_timestamp: u64 = 100;
        envelope[32..40].copy_from_slice(&bad_timestamp.to_be_bytes());

        // Pad with zeros (which won't form valid timestamps)
        let hashes = extract_txset_hashes_from_scp(&envelope);

        // The hash at offset 0 has an invalid timestamp (100), so shouldn't be found
        // Note: This test verifies the timestamp validation, not hash detection
        let found_our_hash = hashes.iter().any(|h| *h == hash);
        assert!(
            !found_our_hash,
            "Should not find hash 0x42... with invalid timestamp 100"
        );
    }

    #[test]
    fn test_extract_txset_hashes_multiple() {
        // Create envelope with multiple valid hash+timestamp pairs
        let mut envelope = vec![0u8; 200];

        let hash1: [u8; 32] = [0x11u8; 32];
        let hash2: [u8; 32] = [0x22u8; 32];
        let timestamp: u64 = 1704067200;

        // First hash at offset 10
        envelope[10..42].copy_from_slice(&hash1);
        envelope[42..50].copy_from_slice(&timestamp.to_be_bytes());

        // Second hash at offset 100
        envelope[100..132].copy_from_slice(&hash2);
        envelope[132..140].copy_from_slice(&timestamp.to_be_bytes());

        let hashes = extract_txset_hashes_from_scp(&envelope);

        assert_eq!(hashes.len(), 2, "Should find two hashes");
        assert!(hashes.contains(&hash1), "Should contain first hash");
        assert!(hashes.contains(&hash2), "Should contain second hash");
    }

    #[test]
    fn test_extract_txset_hashes_dedup() {
        // Create envelope with same hash appearing twice
        let mut envelope = vec![0u8; 200];

        let hash: [u8; 32] = [0x33u8; 32];
        let timestamp: u64 = 1704067200;

        // Same hash at two offsets
        envelope[10..42].copy_from_slice(&hash);
        envelope[42..50].copy_from_slice(&timestamp.to_be_bytes());

        envelope[100..132].copy_from_slice(&hash);
        envelope[132..140].copy_from_slice(&timestamp.to_be_bytes());

        let hashes = extract_txset_hashes_from_scp(&envelope);

        assert_eq!(hashes.len(), 1, "Should deduplicate same hash");
    }
}
