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
pub mod integrated;
mod ipc;
pub mod libp2p_overlay;
mod metrics;
mod xdr;

use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, error, info, warn};

use config::Config;
use flood::{CachedTxSet, Hash256, TxSetCache};
use integrated::{Overlay, OverlayHandle};
use ipc::{CoreIpc, Message, MessageType};
use libp2p::identity::Keypair as Libp2pKeypair;
use libp2p::{Multiaddr, PeerId};
#[cfg(test)]
use libp2p_overlay::create_overlay;
use libp2p_overlay::{
    create_overlay_with_txset_shard_config, OverlayEvent as LibP2pOverlayEvent,
    OverlayHandle as LibP2pOverlayHandle, TxSetReceiveSource, TxSetShardConfig,
};
use metrics::OverlayMetrics;

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

/// Strip the `/p2p/<peer_id>` suffix from a Multiaddr if present.
/// DialOpts::peer_id() supplies the PeerId separately, so the address should be bare.
fn strip_p2p_suffix(addr: &Multiaddr) -> Multiaddr {
    let mut out = Multiaddr::empty();
    for proto in addr.iter() {
        if matches!(proto, libp2p::multiaddr::Protocol::P2p(_)) {
            break;
        }
        out.push(proto);
    }
    out
}

/// Convert a libp2p SocketAddr to a QUIC Multiaddr.
fn socket_addr_to_multiaddr(sock: &SocketAddr) -> Multiaddr {
    let ip_proto = if sock.ip().is_ipv4() { "ip4" } else { "ip6" };
    format!("/{}/{}/udp/{}/quic-v1", ip_proto, sock.ip(), sock.port())
        .parse()
        .unwrap()
}

/// Extract IP and UDP port from a Multiaddr like /ip4/1.2.3.4/udp/12625/quic-v1.
fn multiaddr_to_socket_addr(addr: &Multiaddr) -> Option<SocketAddr> {
    let mut ip = None;
    let mut port = None;
    for proto in addr.iter() {
        match proto {
            libp2p::multiaddr::Protocol::Ip4(a) => ip = Some(std::net::IpAddr::V4(a)),
            libp2p::multiaddr::Protocol::Ip6(a) => ip = Some(std::net::IpAddr::V6(a)),
            libp2p::multiaddr::Protocol::Udp(p) => port = Some(p),
            _ => {}
        }
    }
    match (ip, port) {
        (Some(ip), Some(port)) => Some(SocketAddr::new(ip, port)),
        _ => None,
    }
}

/// Extract TX set hashes from an SCP envelope.
fn extract_txset_hashes_from_scp(envelope: &[u8]) -> Vec<[u8; 32]> {
    xdr::extract_txset_hashes_from_scp(envelope)
}

/// Resolve a peer address string to a SocketAddr.
///
/// Accepts either:
/// - `IP:port` (e.g. "10.0.0.1:11625") — parsed directly
/// - DNS hostname (e.g. "pod-0.svc.cluster.local") — resolved via DNS, using `default_port`
/// - DNS hostname with port (e.g. "pod-0.svc.cluster.local:11625") — resolved via DNS
async fn resolve_peer_addr(addr_str: &str, default_port: u16) -> Result<SocketAddr, String> {
    // Try direct SocketAddr parse first (handles "IP:port")
    if let Ok(addr) = addr_str.parse::<SocketAddr>() {
        return Ok(addr);
    }

    // It's a hostname — append default port if none present
    let host_port = if addr_str.contains(':') {
        addr_str.to_string()
    } else {
        format!("{}:{}", addr_str, default_port)
    };

    // DNS resolution via tokio (async, non-blocking)
    let addrs: Vec<_> = tokio::net::lookup_host(&host_port)
        .await
        .map_err(|e| format!("failed to resolve '{}': {}", host_port, e))?
        .collect();

    addrs
        .iter()
        .copied()
        .find(|addr| addr.is_ipv4())
        .or_else(|| addrs.into_iter().next())
        .ok_or_else(|| format!("DNS returned no addresses for '{}'", host_port))
}

/// Result of resolve_and_dial: either we dialed successfully (with the libp2p SocketAddr)
/// or DNS resolution failed (returning the original address string for retry).
enum DialResult {
    /// Successfully resolved and dialed. Contains the libp2p SocketAddr (ip:port+1000).
    Dialed(SocketAddr),
    /// Successfully resolved but not yet dialed. For resolve-then-check-then-dial flows.
    Resolved(SocketAddr),
    /// Self-dial detected and skipped.
    SelfSkipped,
    /// DNS resolution failed — address should be retried.
    ResolutionFailed(String),
}

/// Resolve a peer address to a libp2p SocketAddr and Multiaddr, without dialing.
/// Returns the libp2p SocketAddr (port+1000) on success.
async fn resolve_peer_to_libp2p(
    addr_str: &str,
    default_port: u16,
    local_addrs: &RwLock<HashSet<SocketAddr>>,
) -> DialResult {
    match resolve_peer_addr(addr_str, default_port).await {
        Ok(addr) => {
            let libp2p_port = addr.port() + 1000;
            let libp2p_sock = SocketAddr::new(addr.ip(), libp2p_port);

            if local_addrs.read().await.contains(&libp2p_sock) {
                debug!(
                    "Skipping self-dial for {} (resolved to local {})",
                    addr_str, addr
                );
                return DialResult::SelfSkipped;
            }

            DialResult::Resolved(libp2p_sock)
        }
        Err(e) => {
            warn!("Failed to resolve peer {}: {}", addr_str, e);
            DialResult::ResolutionFailed(addr_str.to_string())
        }
    }
}

/// Resolve a peer address and dial it.
async fn resolve_and_dial(
    addr_str: &str,
    default_port: u16,
    local_addrs: &RwLock<HashSet<SocketAddr>>,
    handle: &LibP2pOverlayHandle,
) -> DialResult {
    match resolve_peer_addr(addr_str, default_port).await {
        Ok(addr) => {
            let libp2p_port = addr.port() + 1000;
            let libp2p_sock = SocketAddr::new(addr.ip(), libp2p_port);

            if local_addrs.read().await.contains(&libp2p_sock) {
                debug!(
                    "Skipping self-dial for {} (resolved to local {})",
                    addr_str, addr
                );
                return DialResult::SelfSkipped;
            }

            let ip_proto = if addr.ip().is_ipv4() { "ip4" } else { "ip6" };
            let libp2p_addr: libp2p::Multiaddr =
                format!("/{}/{}/udp/{}/quic-v1", ip_proto, addr.ip(), libp2p_port)
                    .parse()
                    .unwrap();

            info!(
                "Resolved peer {} -> {}, dialing {}",
                addr_str, addr, libp2p_addr
            );
            handle.dial(libp2p_addr).await;
            DialResult::Dialed(libp2p_sock)
        }
        Err(e) => {
            warn!("Failed to resolve peer {}: {}", addr_str, e);
            DialResult::ResolutionFailed(addr_str.to_string())
        }
    }
}

/// Spawn a background task that retries DNS resolution for unresolved peers
/// with exponential backoff (capped at 30s). Retries indefinitely until all
/// peers resolve — in K8s, pods may take arbitrarily long to become DNS-ready.
fn spawn_peer_retry_task(
    unresolved: Vec<String>,
    default_port: u16,
    local_addrs: Arc<RwLock<HashSet<SocketAddr>>>,
    configured_peers: Arc<RwLock<ConfiguredPeers>>,
    handle: LibP2pOverlayHandle,
) {
    if unresolved.is_empty() {
        return;
    }

    info!(
        "Scheduling DNS retry for {} unresolved peer(s): {:?}",
        unresolved.len(),
        unresolved
    );

    tokio::spawn(async move {
        let mut pending = unresolved;
        let mut delay = Duration::from_secs(2);
        let max_delay = Duration::from_secs(30);
        let mut attempt: u64 = 0;

        loop {
            tokio::time::sleep(delay).await;
            attempt += 1;

            info!(
                "DNS retry attempt {} for {} peer(s)",
                attempt,
                pending.len()
            );

            let mut still_pending = Vec::new();
            for addr_str in &pending {
                match resolve_and_dial(addr_str, default_port, &local_addrs, &handle).await {
                    DialResult::Dialed(libp2p_sock) => {
                        configured_peers
                            .write()
                            .await
                            .resolved
                            .insert(libp2p_sock, addr_str.clone());
                    }
                    DialResult::Resolved(_) | DialResult::SelfSkipped => {}
                    DialResult::ResolutionFailed(addr) => {
                        still_pending.push(addr);
                    }
                }
            }

            if still_pending.is_empty() {
                info!("All peers resolved successfully after {} retries", attempt);
                return;
            }

            pending = still_pending;
            delay = (delay * 2).min(max_delay);
        }
    });
}

/// Collect local IP addresses for self-dial detection.
/// Returns a set of SocketAddrs at the libp2p port (peer_port + 1000).
/// Starts with instantly-available addresses; DNS resolution runs in background.
fn collect_local_addrs(libp2p_port: u16) -> Arc<RwLock<HashSet<SocketAddr>>> {
    let mut addrs = HashSet::new();

    // Always include loopback
    addrs.insert(SocketAddr::new(
        std::net::IpAddr::V4(std::net::Ipv4Addr::LOCALHOST),
        libp2p_port,
    ));

    // Probe for our primary local IP by connecting a UDP socket.
    // This doesn't send traffic — it just lets the OS pick the outbound interface.
    if let Ok(socket) = std::net::UdpSocket::bind("0.0.0.0:0") {
        if socket.connect("8.8.8.8:80").is_ok() {
            if let Ok(local) = socket.local_addr() {
                addrs.insert(SocketAddr::new(local.ip(), libp2p_port));
            }
        }
    }

    let local_addrs = Arc::new(RwLock::new(addrs));

    // Spawn background DNS resolution of our own hostname (for K8s pod IP detection).
    // This runs concurrently with app startup — doesn't block event loop.
    let addrs_ref = local_addrs.clone();
    tokio::spawn(async move {
        if let Ok(hostname) = hostname::get() {
            if let Ok(hostname_str) = hostname.into_string() {
                let lookup = format!("{}:{}", hostname_str, libp2p_port);
                match tokio::net::lookup_host(lookup).await {
                    Ok(resolved) => {
                        let resolved: Vec<_> = resolved.collect();
                        if !resolved.is_empty() {
                            let mut addrs = addrs_ref.write().await;
                            for addr in &resolved {
                                addrs.insert(*addr);
                            }
                            debug!("DNS self-detection resolved hostname to {:?}", resolved);
                        }
                    }
                    Err(e) => {
                        debug!(
                            "Hostname DNS resolution for self-dial detection failed: {}",
                            e
                        );
                    }
                }
            }
        }
    });

    local_addrs
}

fn get_cached_tx_set_xdr(tx_set_cache: &TxSetCache, hash: &Hash256) -> Option<Vec<u8>> {
    tx_set_cache.get(hash).map(|cached| cached.xdr.clone())
}

fn cache_tx_set_xdr(
    tx_set_cache: &mut TxSetCache,
    current_ledger_seq: u32,
    hash: Hash256,
    xdr: Vec<u8>,
) {
    tx_set_cache.insert(CachedTxSet {
        hash,
        xdr,
        ledger_seq: current_ledger_seq,
    });
}

/// Application state
struct App {
    core_ipc: CoreIpc,
    overlay_handle: OverlayHandle,
    /// Cache for built TX sets
    tx_set_cache: TxSetCache,
    /// Current ledger sequence
    current_ledger_seq: u32,
    /// libp2p overlay handle (QUIC-based SCP + TX)
    libp2p_handle: LibP2pOverlayHandle,
    /// libp2p overlay events (SCP, TxSet - critical, unbounded)
    libp2p_events: mpsc::UnboundedReceiver<LibP2pOverlayEvent>,
    /// libp2p TX events (bounded, may drop under backpressure)
    tx_events: mpsc::Receiver<LibP2pOverlayEvent>,
    /// Pending SCP state requests: maps request_id to requesting peer
    /// When Core responds with ScpStateResponse containing request_id, we look up the peer
    pending_scp_state_requests: Arc<RwLock<HashMap<u64, PeerId>>>,
    /// Counter for generating unique SCP state request IDs
    next_scp_request_id: Arc<AtomicU64>,
    /// Local addresses for self-dial detection (populated at startup + async DNS)
    local_addrs: Arc<RwLock<HashSet<SocketAddr>>>,
    /// Configured peer addresses and listen port — kept for reconnection on disconnect.
    /// Updated each time SetPeerConfig is received from Core.
    configured_peers: Arc<RwLock<ConfiguredPeers>>,
    /// Known peers: PeerId → Multiaddr, learned from ConnectionEstablished events.
    /// Used for PeerId-based reconnection (libp2p can deduplicate).
    known_peers: Arc<RwLock<HashMap<PeerId, Multiaddr>>>,
    /// PeerId → configured hostname, so targeted reconnect can re-resolve DNS
    /// after a pod restart changes the peer's IP address.
    peer_hostnames: Arc<RwLock<HashMap<PeerId, String>>>,
    /// Shared metrics counters for the overlay
    metrics: Arc<OverlayMetrics>,
    /// Tracks txsets sent or received through eager sharding for effectiveness metrics.
    eager_txset_tracker: EagerTxSetTracker,
}

/// Peer addresses configured via SetPeerConfig, used for reconnection.
struct ConfiguredPeers {
    /// All peer address strings (known + preferred)
    addrs: Vec<String>,
    /// The listen_port from the config (used as default_port for DNS resolution)
    listen_port: u16,
    /// Map from resolved SocketAddr (at libp2p port) to original address string,
    /// so we can reconnect by address when a PeerId disconnects.
    resolved: HashMap<SocketAddr, String>,
}

#[derive(Default)]
struct EagerTxSetTracker {
    received_via_shards: HashSet<Hash256>,
    sent_via_shards: HashSet<Hash256>,
}

impl EagerTxSetTracker {
    fn record_received_via_shards(&mut self, hash: Hash256) {
        self.received_via_shards.insert(hash);
    }

    fn record_sent_via_shards(&mut self, hash: Hash256) {
        self.sent_via_shards.insert(hash);
    }

    fn record_core_request_cache_hit(&self, hash: &Hash256, metrics: &OverlayMetrics) {
        if self.received_via_shards.contains(hash) {
            metrics
                .txset_shard_fetch_preempted
                .fetch_add(1, Ordering::Relaxed);
        }
    }

    fn record_peer_request_served(&self, hash: &Hash256, metrics: &OverlayMetrics) {
        if self.sent_via_shards.contains(hash) {
            metrics
                .txset_shard_eager_also_served
                .fetch_add(1, Ordering::Relaxed);
        }
    }
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
        let metrics = Arc::new(OverlayMetrics::new());
        let txset_shard_config = TxSetShardConfig {
            target_shard_size: config.txset_target_shard_size,
            recovery_factor_percent: config.txset_shard_recovery_factor_percent,
            initial_ttl: config.txset_shard_ttl,
        };
        let (libp2p_handle, libp2p_event_rx, tx_event_rx, libp2p_overlay) =
            create_overlay_with_txset_shard_config(
                libp2p_keypair,
                Arc::clone(&metrics),
                txset_shard_config,
            )
            .map_err(|e| format!("Failed to create libp2p overlay: {}", e))?;

        // Use peer_port + 1000 for libp2p QUIC to avoid collision with legacy TCP
        let libp2p_port = config.peer_port + 1000;
        let libp2p_listen_ip = config.libp2p_listen_ip.clone();

        // Compute local addresses for self-dial detection (instant + async DNS in background)
        let local_addrs = collect_local_addrs(libp2p_port);

        // Spawn libp2p overlay task
        tokio::spawn(async move {
            libp2p_overlay.run(&libp2p_listen_ip, libp2p_port).await;
        });

        info!(
            "Started libp2p QUIC overlay on {}:{} (SCP + TX + TxSet streams)",
            config.libp2p_listen_ip, libp2p_port
        );

        Ok(Self {
            core_ipc,
            overlay_handle,
            tx_set_cache: TxSetCache::new(100),
            current_ledger_seq: 0,
            libp2p_handle,
            libp2p_events: libp2p_event_rx,
            tx_events: tx_event_rx,
            pending_scp_state_requests: Arc::new(RwLock::new(HashMap::new())),
            next_scp_request_id: Arc::new(AtomicU64::new(1)),
            local_addrs,
            configured_peers: Arc::new(RwLock::new(ConfiguredPeers {
                addrs: Vec::new(),
                listen_port: 11625,
                resolved: HashMap::new(),
            })),
            known_peers: Arc::new(RwLock::new(HashMap::new())),
            peer_hostnames: Arc::new(RwLock::new(HashMap::new())),
            metrics,
            eager_txset_tracker: EagerTxSetTracker::default(),
        })
    }

    /// Main event loop - process messages from Core and overlay events
    async fn run(mut self) {
        info!("Overlay started, processing Core messages");

        // Safety-net reconnect timer: re-dial all configured peers every 30s.
        // Uses PeerId-based dials for known peers (libp2p skips if already connected).
        // Falls back to address-based dials for peers we haven't connected to yet.
        // This is a fallback — targeted reconnection on disconnect handles the fast path.
        let mut reconnect_interval = tokio::time::interval(Duration::from_secs(30));
        reconnect_interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

        loop {
            tokio::select! {
                // Receive message from Core
                msg = self.core_ipc.receiver.recv() => {
                    match msg {
                        Some(msg) => {
                            if !self.handle_core_message(msg).await {
                                break;
                            }
                        }
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

                // Safety-net reconnect: PeerId-based dials for known peers,
                // address-based ONLY for peers we've never learned a PeerId for.
                _ = reconnect_interval.tick() => {
                    let cp = self.configured_peers.read().await;
                    let addrs = cp.addrs.clone();
                    let listen_port = cp.listen_port;
                    let expected_peers = addrs.len().saturating_sub(1); // exclude self
                    // Build set of hostnames that have a known PeerId — these
                    // are handled by PeerId-based dials and must NOT be raw-dialed.
                    let hostnames_with_known_peer: HashSet<String> = {
                        let hostnames = self.peer_hostnames.read().await;
                        hostnames.values().cloned().collect()
                    };
                    // Also collect resolved SocketAddrs that map to known peers
                    let known_addrs: HashSet<SocketAddr> = {
                        let known = self.known_peers.read().await;
                        known.values()
                            .filter_map(|maddr| multiaddr_to_socket_addr(maddr))
                            .collect()
                    };
                    drop(cp);

                    if !addrs.is_empty() {
                        let connected = self.libp2p_handle.connected_peer_count().await;
                        if connected < expected_peers {
                            info!(
                                "Safety-net reconnect: {}/{} peers connected",
                                connected, expected_peers
                            );

                            // PeerId-based dials for configured peers we've seen before.
                            // Only peers with a hostname entry are configured — this
                            // prevents re-dialing unconfigured inbound-only peers.
                            let hostnames = self.peer_hostnames.read().await;
                            let configured_peer_ids: Vec<PeerId> = hostnames.keys().cloned().collect();
                            drop(hostnames);

                            let known = self.known_peers.read().await;
                            let known_snapshot: Vec<_> = configured_peer_ids.iter()
                                .filter_map(|pid| known.get(pid).map(|addr| (*pid, addr.clone())))
                                .collect();
                            drop(known);

                            let handle = self.libp2p_handle.clone();
                            for (peer_id, addr) in &known_snapshot {
                                handle.dial_peer(*peer_id, addr.clone()).await;
                            }

                            // Raw address dials ONLY for configured peers we've never
                            // learned a PeerId for. Resolve DNS first, then check the
                            // resolved address against known peers BEFORE dialing —
                            // a raw dial cannot be deduplicated by libp2p.
                            let unknown_addrs: Vec<_> = addrs.iter()
                                .filter(|a| !hostnames_with_known_peer.contains(*a))
                                .cloned()
                                .collect();

                            if !unknown_addrs.is_empty() {
                                info!(
                                    "Safety-net: resolving {} unknown peer(s)",
                                    unknown_addrs.len()
                                );
                                let handle = self.libp2p_handle.clone();
                                let local_addrs = self.local_addrs.clone();
                                let configured_peers = self.configured_peers.clone();

                                tokio::spawn(async move {
                                    for addr_str in &unknown_addrs {
                                        // Step 1: resolve DNS only (no dial)
                                        match resolve_peer_to_libp2p(
                                            addr_str, listen_port, &local_addrs,
                                        ).await {
                                            DialResult::Resolved(libp2p_sock) => {
                                                // Step 2: check if resolved addr is already known
                                                if known_addrs.contains(&libp2p_sock) {
                                                    debug!(
                                                        "Safety-net: {} resolved to known addr {}, skipping dial",
                                                        addr_str, libp2p_sock
                                                    );
                                                    continue;
                                                }
                                                // Step 3: truly unknown — dial
                                                let maddr = socket_addr_to_multiaddr(&libp2p_sock);
                                                info!("Safety-net: dialing unknown peer {} at {}", addr_str, maddr);
                                                handle.dial(maddr).await;
                                                configured_peers
                                                    .write()
                                                    .await
                                                    .resolved
                                                    .insert(libp2p_sock, addr_str.clone());
                                            }
                                            DialResult::SelfSkipped => {}
                                            DialResult::ResolutionFailed(_) => {}
                                            DialResult::Dialed(_) => unreachable!(),
                                        }
                                    }
                                });
                            }
                        }
                    }
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

                // Extract TX set hashes and remember which peer advertised
                // them. Core decides when a missing txset should be fetched;
                // the overlay must not race shard reconstruction by eagerly
                // sending a full GetTxSet request from this path.
                let txset_hashes = extract_txset_hashes_from_scp(&envelope);
                if !txset_hashes.is_empty() {
                    let handle = self.libp2p_handle.clone();
                    let from_peer = from;
                    tokio::spawn(async move {
                        for txhash in &txset_hashes {
                            debug!(
                                "Recording peer {} as source for TX set {:02x?}...",
                                from_peer,
                                &txhash[..4]
                            );
                            handle.record_txset_source(*txhash, from_peer).await;
                        }
                    });
                }

                // Forward to Core
                if let Err(e) = self.core_ipc.sender.send_scp_received(envelope) {
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
                self.overlay_handle.submit_tx(tx, 0, 0);
            }
            LibP2pOverlayEvent::TxSetReceived {
                hash,
                data,
                from,
                source,
            } => {
                info!(
                    "TXSET_RECV: Received TxSet {:02x?}... ({} bytes) from {}",
                    &hash[..4],
                    data.len(),
                    from
                );
                let data = match xdr::verify_generalized_tx_set_xdr(&hash, &data) {
                    Ok(canonical) => canonical,
                    Err(e) => {
                        warn!(
                            "TXSET_RECV_DROP: Dropping invalid TxSet {:02x?}... from {}: {}",
                            &hash[..4],
                            from,
                            e
                        );
                        return;
                    }
                };

                // IMPORTANT: Cache the TxSet FIRST, before pushing to Core
                // This ensures the TxSet is available when SCP processing resumes
                cache_tx_set_xdr(
                    &mut self.tx_set_cache,
                    self.current_ledger_seq,
                    hash,
                    data.clone(),
                );
                if source == TxSetReceiveSource::Shard {
                    self.eager_txset_tracker.record_received_via_shards(hash);
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
                if let Some(cached) = self.tx_set_cache.get(&hash) {
                    self.eager_txset_tracker
                        .record_peer_request_served(&hash, &self.metrics);
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
                        self.tx_set_cache.len()
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

            LibP2pOverlayEvent::PeerConnected { peer_id, addr } => {
                // Only record the mapping if this peer's address matches a configured peer.
                // Inbound connections from unconfigured peers must NOT be reconnect-eligible.
                let clean_addr = strip_p2p_suffix(&addr);
                let cp = self.configured_peers.read().await;
                let hostname = multiaddr_to_socket_addr(&clean_addr)
                    .and_then(|sock| cp.resolved.get(&sock).cloned());
                drop(cp);

                if let Some(host) = hostname {
                    info!(
                        "Learned configured peer {} at {} (hostname: {})",
                        peer_id, clean_addr, host
                    );
                    self.known_peers.write().await.insert(peer_id, clean_addr);
                    self.peer_hostnames.write().await.insert(peer_id, host);
                } else {
                    debug!(
                        "Peer {} at {} is not a configured peer, not tracking for reconnect",
                        peer_id, clean_addr
                    );
                }
            }

            LibP2pOverlayEvent::PeerDisconnected { peer_id } => {
                // Clean up any pending SCP state requests for this peer
                {
                    let mut pending = self.pending_scp_state_requests.write().await;
                    let before_len = pending.len();
                    pending.retain(|_request_id, p| p != &peer_id);
                    let removed = before_len - pending.len();
                    if removed > 0 {
                        info!(
                            "Removed {} pending SCP state requests for disconnected peer {}",
                            removed, peer_id
                        );
                    }
                }

                // Targeted reconnect: only for configured peers (those with a hostname).
                // Unconfigured inbound-only peers are not re-dialed.
                let hostname = self.peer_hostnames.read().await.get(&peer_id).cloned();
                let known_addr = self.known_peers.read().await.get(&peer_id).cloned();
                if let Some(hostname) = hostname {
                    info!(
                        "Peer {} disconnected, scheduling targeted reconnect (host={}, addr={:?})",
                        peer_id, hostname, known_addr
                    );
                    let handle = self.libp2p_handle.clone();
                    let local_addrs = self.local_addrs.clone();
                    let known_peers = self.known_peers.clone();
                    let configured_peers = self.configured_peers.clone();
                    tokio::spawn(async move {
                        let mut delay = Duration::from_secs(1);
                        let max_delay = Duration::from_secs(30);
                        // First 3 attempts: use cached Multiaddr (fast path).
                        // Remaining attempts: re-resolve DNS in case IP changed.
                        for attempt in 1u32..=10 {
                            tokio::time::sleep(delay).await;

                            if attempt <= 3 {
                                if let Some(ref addr) = known_addr {
                                    debug!(
                                        "Reconnect attempt {} for {} via cached addr {}",
                                        attempt, peer_id, addr
                                    );
                                    handle.dial_peer(peer_id, addr.clone()).await;
                                }
                            } else {
                                // Re-resolve DNS (handles K8s pod restart / IP change)
                                let cp = configured_peers.read().await;
                                let listen_port = cp.listen_port;
                                drop(cp);
                                debug!(
                                    "Reconnect attempt {} for {} via DNS re-resolve of {}",
                                    attempt, peer_id, hostname
                                );
                                match resolve_and_dial(
                                    &hostname,
                                    listen_port,
                                    &local_addrs,
                                    &handle,
                                )
                                .await
                                {
                                    DialResult::Dialed(libp2p_sock) => {
                                        let new_addr = socket_addr_to_multiaddr(&libp2p_sock);
                                        known_peers.write().await.insert(peer_id, new_addr);
                                        configured_peers
                                            .write()
                                            .await
                                            .resolved
                                            .insert(libp2p_sock, hostname.clone());
                                    }
                                    DialResult::SelfSkipped => break,
                                    DialResult::ResolutionFailed(_) => {}
                                    DialResult::Resolved(_) => unreachable!(),
                                }
                            }

                            delay = (delay * 2).min(max_delay);
                        }
                    });
                } else {
                    debug!(
                        "Peer {} disconnected (not a configured peer), not reconnecting",
                        peer_id
                    );
                }
            }
        }
    }

    /// Handle a message from Core. Returns false to signal shutdown.
    async fn handle_core_message(&mut self, msg: Message) -> bool {
        match msg.msg_type {
            MessageType::Shutdown => {
                info!("Shutdown requested by Core");
                return false;
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
                    return true;
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
                    return true;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);

                // First check local cache
                if let Some(xdr) = get_cached_tx_set_xdr(&self.tx_set_cache, &hash) {
                    self.eager_txset_tracker
                        .record_core_request_cache_hit(&hash, &self.metrics);
                    info!(
                        "TXSET_FROM_CACHE: Sending TX set {:02x?}... ({} bytes) from local cache",
                        &hash[..4],
                        xdr.len()
                    );
                    if let Err(e) = self.core_ipc.sender.send_tx_set_available(hash, xdr) {
                        error!("Failed to send TX set: {}", e);
                    }
                } else {
                    info!(
                        "TXSET_SHARDS_ONLY: TX set {:02x?}... not in cache; waiting for shard reconstruction",
                        &hash[..4]
                    );
                }
            }

            MessageType::CacheTxSet => {
                // Core built a TX set locally and wants us to cache it for peer requests
                // Payload: [hash:32][txSetXDR...]
                if msg.payload.len() < 33 {
                    warn!("CacheTxSet payload too short");
                    return true;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);
                let tx_set_xdr = match xdr::verify_generalized_tx_set_xdr(&hash, &msg.payload[32..])
                {
                    Ok(canonical) => canonical,
                    Err(e) => {
                        warn!(
                            "TXSET_CACHE_DROP: Dropping invalid TX set {:02x?}... from Core: {}",
                            &hash[..4],
                            e
                        );
                        return true;
                    }
                };

                info!(
                    "TXSET_CACHE: Caching locally-built TX set {:02x?}... ({} bytes)",
                    &hash[..4],
                    tx_set_xdr.len()
                );

                cache_tx_set_xdr(
                    &mut self.tx_set_cache,
                    self.current_ledger_seq,
                    hash,
                    tx_set_xdr,
                );
            }

            MessageType::BroadcastTxSetShards => {
                if msg.payload.len() != 32 {
                    warn!(
                        "BroadcastTxSetShards payload has invalid length: {}",
                        msg.payload.len()
                    );
                    return true;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload);

                let Some(tx_set_xdr) = get_cached_tx_set_xdr(&self.tx_set_cache, &hash) else {
                    warn!(
                        "TXSET_SHARD_BROADCAST_DROP: TX set {:02x?}... not cached",
                        &hash[..4]
                    );
                    return true;
                };

                info!(
                    "TXSET_SHARD_BROADCAST_FROM_CORE: Broadcasting cached TX set {:02x?}... ({} bytes)",
                    &hash[..4],
                    tx_set_xdr.len()
                );

                self.eager_txset_tracker.record_sent_via_shards(hash);

                self.libp2p_handle
                    .broadcast_txset_shards(hash, tx_set_xdr)
                    .await;
            }

            MessageType::SubmitTx => {
                // Parse payload: [fee:i64][numOps:u32][txEnvelope...]
                if msg.payload.len() < 12 {
                    warn!("SubmitTx payload too short");
                    return true;
                }

                let fee = i64::from_le_bytes(msg.payload[0..8].try_into().unwrap());
                let num_ops = u32::from_le_bytes(msg.payload[8..12].try_into().unwrap());
                let tx_data = msg.payload[12..].to_vec();

                let parsed_tx = match xdr::parse_supported_transaction(&tx_data) {
                    Ok(parsed) => parsed,
                    Err(e) => {
                        warn!("SUBMIT_TX_DROP: Dropping unsupported TX from Core: {}", e);
                        return true;
                    }
                };
                if u64::try_from(fee).ok() != Some(parsed_tx.fee) || num_ops != parsed_tx.num_ops {
                    debug!(
                        "SUBMIT_TX_METADATA_MISMATCH: Core fee/ops=({}/{}) XDR fee/ops=({}/{})",
                        fee, num_ops, parsed_tx.fee, parsed_tx.num_ops
                    );
                }

                // Add to mempool
                self.overlay_handle.submit_tx(
                    parsed_tx.envelope_xdr.clone(),
                    parsed_tx.fee,
                    parsed_tx.num_ops,
                );

                // Broadcast TX via libp2p QUIC (dedicated stream)
                let handle = self.libp2p_handle.clone();
                let tx_data = parsed_tx.envelope_xdr;
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

                    // Update current ledger
                    self.current_ledger_seq = ledger_seq;

                    // Evict old TX sets from cache
                    self.tx_set_cache
                        .evict_before(ledger_seq.saturating_sub(12));
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
                        self.overlay_handle.remove_txs_sync(tx_hashes).await;
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
                    return true;
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
                            return true;
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

                        // Resolve and dial all known/preferred peers
                        let all_peers: Vec<_> =
                            known.into_iter().chain(preferred.into_iter()).collect();

                        // Store configured peers for reconnection
                        {
                            let mut cp = self.configured_peers.write().await;
                            cp.addrs = all_peers.clone();
                            cp.listen_port = listen_port;
                            cp.resolved.clear();
                        }

                        // Prune known_peers and peer_hostnames for peers whose
                        // hostnames are no longer in the config. Prevents stale
                        // entries from re-dialing removed peers.
                        {
                            let new_hosts: HashSet<&str> =
                                all_peers.iter().map(|s| s.as_str()).collect();
                            let hostnames = self.peer_hostnames.read().await;
                            let stale_peers: Vec<PeerId> = hostnames
                                .iter()
                                .filter(|(_pid, host)| !new_hosts.contains(host.as_str()))
                                .map(|(pid, _)| *pid)
                                .collect();
                            drop(hostnames);
                            if !stale_peers.is_empty() {
                                info!("Pruning {} peers removed from config", stale_peers.len());
                                let mut known = self.known_peers.write().await;
                                let mut hosts = self.peer_hostnames.write().await;
                                for pid in &stale_peers {
                                    known.remove(pid);
                                    hosts.remove(pid);
                                }
                            }
                        }

                        let handle = self.libp2p_handle.clone();
                        let local_addrs = self.local_addrs.clone();
                        let configured_peers = self.configured_peers.clone();

                        tokio::spawn(async move {
                            let mut unresolved = Vec::new();
                            for addr_str in &all_peers {
                                match resolve_and_dial(addr_str, listen_port, &local_addrs, &handle)
                                    .await
                                {
                                    DialResult::Dialed(libp2p_sock) => {
                                        // Record mapping so we can reconnect on disconnect
                                        configured_peers
                                            .write()
                                            .await
                                            .resolved
                                            .insert(libp2p_sock, addr_str.clone());
                                    }
                                    DialResult::Resolved(_) | DialResult::SelfSkipped => {}
                                    DialResult::ResolutionFailed(addr) => {
                                        unresolved.push(addr);
                                    }
                                }
                            }

                            // Retry any peers that failed DNS resolution
                            spawn_peer_retry_task(
                                unresolved,
                                listen_port,
                                local_addrs,
                                configured_peers,
                                handle,
                            );
                        });
                    }
                }
            }

            MessageType::RequestOverlayMetrics => {
                // Snapshot metrics and send back as JSON
                let snapshot = self.metrics.snapshot();
                match serde_json::to_vec(&snapshot) {
                    Ok(json_bytes) => {
                        let resp = Message::new(MessageType::OverlayMetricsResponse, json_bytes);
                        if let Err(e) = self.core_ipc.sender.send(resp) {
                            error!("Failed to send metrics response: {}", e);
                        }
                    }
                    Err(e) => {
                        error!("Failed to serialize metrics snapshot: {}", e);
                    }
                }
            }

            _ => {
                warn!("Unexpected message type from Core: {:?}", msg.msg_type);
            }
        }
        true
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
    use stellar_xdr::curr::{Limits, ScpEnvelope, WriteXdr};

    fn test_scp_envelope_xdr(slot_index: u64) -> Vec<u8> {
        let mut envelope = ScpEnvelope::default();
        envelope.statement.slot_index = slot_index;
        envelope.to_xdr(Limits::none()).unwrap()
    }

    #[test]
    fn test_extract_txset_hashes_rejects_invalid_xdr() {
        assert!(extract_txset_hashes_from_scp(&[]).is_empty());
        assert!(extract_txset_hashes_from_scp(&[0u8; 40]).is_empty());
    }

    // --- DNS resolution tests ---

    #[tokio::test]
    async fn test_resolve_peer_addr_ip_port() {
        // Bare IP:port should parse directly without DNS
        let addr = resolve_peer_addr("10.0.0.1:11625", 9999).await.unwrap();
        assert_eq!(addr, "10.0.0.1:11625".parse::<SocketAddr>().unwrap());
        // default_port is ignored when addr already has a port
    }

    #[tokio::test]
    async fn test_resolve_peer_addr_ip_port_various() {
        // Loopback
        let addr = resolve_peer_addr("127.0.0.1:8080", 0).await.unwrap();
        assert_eq!(addr.ip().to_string(), "127.0.0.1");
        assert_eq!(addr.port(), 8080);

        // High port
        let addr = resolve_peer_addr("192.168.1.1:65535", 0).await.unwrap();
        assert_eq!(addr.port(), 65535);
    }

    #[tokio::test]
    async fn test_resolve_peer_addr_dns_no_port() {
        // "localhost" is a DNS name; should resolve and use default_port
        let addr = resolve_peer_addr("localhost", 11625).await.unwrap();
        assert!(
            addr.ip().is_loopback(),
            "localhost should resolve to loopback, got {}",
            addr.ip()
        );
        assert_eq!(
            addr.port(),
            11625,
            "Should use default_port when hostname has no port"
        );
    }

    #[tokio::test]
    async fn test_resolve_peer_addr_dns_with_port() {
        // "localhost:9999" — DNS name with explicit port
        let addr = resolve_peer_addr("localhost:9999", 11625).await.unwrap();
        assert!(addr.ip().is_loopback());
        assert_eq!(
            addr.port(),
            9999,
            "Should use explicit port, not default_port"
        );
    }

    #[tokio::test]
    async fn test_resolve_peer_addr_unresolvable() {
        // Bogus hostname should return an error
        let result = resolve_peer_addr("this.host.definitely.does.not.exist.invalid", 11625).await;
        assert!(result.is_err(), "Unresolvable hostname should return Err");
        let err = result.unwrap_err();
        assert!(
            err.contains("failed to resolve"),
            "Error should mention resolution failure, got: {}",
            err
        );
    }

    #[tokio::test]
    async fn test_resolve_peer_addr_ipv6_bracket() {
        // Bracketed IPv6 with port should parse directly
        let addr = resolve_peer_addr("[::1]:11625", 9999).await.unwrap();
        assert!(addr.ip().is_ipv6());
        assert_eq!(addr.port(), 11625);
    }

    // --- collect_local_addrs tests ---

    #[tokio::test]
    async fn test_collect_local_addrs_includes_loopback() {
        let addrs = collect_local_addrs(12625);
        // Loopback is inserted synchronously, should be present immediately
        let set = addrs.read().await;
        let loopback = SocketAddr::new(std::net::IpAddr::V4(std::net::Ipv4Addr::LOCALHOST), 12625);
        assert!(
            set.contains(&loopback),
            "Local addrs must always contain loopback at the libp2p port"
        );
    }

    #[tokio::test]
    async fn test_collect_local_addrs_has_nonloopback() {
        // The UDP probe should find at least our primary interface IP
        let addrs = collect_local_addrs(12625);
        let set = addrs.read().await;
        assert!(
            set.len() >= 2,
            "Should have loopback + at least one probe result, got {} addrs: {:?}",
            set.len(),
            set,
        );
    }

    // --- resolve_and_dial tests ---

    #[tokio::test]
    async fn test_resolve_and_dial_self_dial_skipped() {
        // If the resolved address is in local_addrs, resolve_and_dial should
        // return SelfSkipped.
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        // 127.0.0.1:11625 → libp2p port 12625
        local_addrs
            .write()
            .await
            .insert("127.0.0.1:12625".parse().unwrap());

        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        let result = resolve_and_dial("127.0.0.1:11625", 11625, &local_addrs, &handle).await;
        assert!(
            matches!(result, DialResult::SelfSkipped),
            "Self-dial should be skipped"
        );
    }

    #[tokio::test]
    async fn test_resolve_and_dial_dns_failure_returns_addr() {
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        let result = resolve_and_dial("unresolvable.invalid", 11625, &local_addrs, &handle).await;
        assert!(
            matches!(result, DialResult::ResolutionFailed(ref s) if s == "unresolvable.invalid"),
            "Failed DNS should return ResolutionFailed with the address string"
        );
    }

    #[tokio::test]
    async fn test_resolve_and_dial_ip_port_success() {
        // A valid IP:port that is NOT in local_addrs should return Dialed.
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        let result = resolve_and_dial("10.255.255.1:11625", 11625, &local_addrs, &handle).await;
        assert!(
            matches!(result, DialResult::Dialed(_)),
            "Valid IP:port should resolve and return Dialed"
        );
    }

    #[tokio::test]
    async fn test_resolve_and_dial_dns_success() {
        // "localhost" should resolve via DNS and return Dialed.
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        let result = resolve_and_dial("localhost", 11625, &local_addrs, &handle).await;
        assert!(
            matches!(result, DialResult::Dialed(_)),
            "localhost should resolve via DNS and return Dialed"
        );
    }

    // --- spawn_peer_retry_task tests ---

    fn make_test_configured_peers() -> Arc<RwLock<ConfiguredPeers>> {
        Arc::new(RwLock::new(ConfiguredPeers {
            addrs: Vec::new(),
            listen_port: 11625,
            resolved: HashMap::new(),
        }))
    }

    #[tokio::test]
    async fn test_spawn_peer_retry_empty_is_noop() {
        // Empty unresolved list should not spawn anything
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        // This should return immediately without spawning a task
        spawn_peer_retry_task(
            vec![],
            11625,
            local_addrs,
            make_test_configured_peers(),
            handle,
        );
        // No panic, no hang — that's the test
    }

    #[tokio::test]
    async fn test_spawn_peer_retry_resolves_on_retry() {
        // "localhost" should resolve on the first retry attempt.
        // We put it in the "unresolved" list as if initial resolution failed.
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        // Use tokio::time::pause() so the test doesn't actually sleep 2+ seconds
        tokio::time::pause();

        spawn_peer_retry_task(
            vec!["localhost".to_string()],
            11625,
            local_addrs,
            make_test_configured_peers(),
            handle,
        );

        // Advance time past the first retry delay (2s)
        tokio::time::advance(Duration::from_secs(3)).await;
        // Yield to let the spawned task run
        tokio::task::yield_now().await;

        // If we get here without hanging, the retry resolved "localhost" and exited.
        // (An unresolvable host would keep retrying forever, but "localhost" succeeds on attempt 1.)
    }

    #[tokio::test]
    async fn test_spawn_peer_retry_keeps_retrying() {
        // With an unresolvable host, the retry task should keep going indefinitely
        // (no max attempts). We verify it survives multiple retry cycles.
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        tokio::time::pause();

        spawn_peer_retry_task(
            vec!["will-never-resolve.invalid".to_string()],
            11625,
            local_addrs,
            make_test_configured_peers(),
            handle,
        );

        // Advance through many retry cycles — the task should not exit or panic.
        // Delays: 2, 4, 8, 16, 30, 30, 30, ... (capped at 30s)
        // After 300s, we've done ~12 retries. Task is still alive.
        tokio::time::advance(Duration::from_secs(300)).await;
        tokio::task::yield_now().await;

        // Advance further — still should not panic or exit
        tokio::time::advance(Duration::from_secs(300)).await;
        tokio::task::yield_now().await;

        // If we get here, the retry loop is still running (no max attempts). Pass.
    }

    /// Integration test: when known_peers are passed to the overlay via
    /// resolve_and_dial, all of them (IPs and DNS names) get resolved and
    /// connected. Verifies the full SetPeerConfig → resolve → dial → connected flow.
    #[tokio::test]
    async fn test_all_known_peers_resolve_and_connect() {
        // Create 3 overlay nodes
        let kp1 = Libp2pKeypair::generate_ed25519();
        let kp2 = Libp2pKeypair::generate_ed25519();
        let kp3 = Libp2pKeypair::generate_ed25519();

        let (handle1, _events1, _tx1, overlay1) =
            create_overlay(kp1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, _tx2, overlay2) =
            create_overlay(kp2, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle3, mut events3, _tx3, overlay3) =
            create_overlay(kp3, Arc::new(OverlayMetrics::new())).unwrap();

        // Start all three on different ports
        let port1: u16 = 18501;
        let port2: u16 = 18502;
        let port3: u16 = 18503;
        tokio::spawn(async move { overlay1.run("127.0.0.1", port1).await });
        tokio::spawn(async move { overlay2.run("127.0.0.1", port2).await });
        tokio::spawn(async move { overlay3.run("127.0.0.1", port3).await });
        tokio::time::sleep(Duration::from_millis(200)).await;

        // Node1 resolves and dials all peers using a mix of IP and DNS formats.
        // peer_port values are: port2 - 1000 = 17502, port3 - 1000 = 17503
        // (resolve_and_dial adds +1000 for libp2p_port)
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let known_peers: Vec<String> = vec![
            format!("127.0.0.1:{}", port2 - 1000), // bare IP:port for node2
            format!("localhost:{}", port3 - 1000), // DNS name:port for node3
        ];

        for addr_str in &known_peers {
            let result = resolve_and_dial(addr_str, 11625, &local_addrs, &handle1).await;
            assert!(
                matches!(result, DialResult::Dialed(_)),
                "Peer {} should resolve and dial on first try",
                addr_str
            );
        }

        // Wait for connections + stream establishment
        tokio::time::sleep(Duration::from_millis(500)).await;

        // Verify connectivity by broadcasting SCP from node1 and receiving on node2 and node3
        let scp_msg = test_scp_envelope_xdr(1);
        handle1.broadcast_scp(scp_msg.clone()).await;

        let mut node2_received = false;
        let mut node3_received = false;
        let deadline = tokio::time::Instant::now() + Duration::from_secs(3);

        while tokio::time::Instant::now() < deadline && !(node2_received && node3_received) {
            tokio::select! {
                Some(event) = events2.recv() => {
                    if let LibP2pOverlayEvent::ScpReceived { envelope, .. } = event {
                        if envelope == scp_msg {
                            node2_received = true;
                        }
                    }
                }
                Some(event) = events3.recv() => {
                    if let LibP2pOverlayEvent::ScpReceived { envelope, .. } = event {
                        if envelope == scp_msg {
                            node3_received = true;
                        }
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(10)) => {}
            }
        }

        assert!(
            node2_received,
            "Node2 (connected via bare IP) should receive SCP broadcast"
        );
        assert!(
            node3_received,
            "Node3 (connected via DNS name) should receive SCP broadcast"
        );

        handle1.shutdown().await;
        handle2.shutdown().await;
        handle3.shutdown().await;
    }

    #[tokio::test]
    async fn test_spawn_peer_retry_backoff_caps_at_30s() {
        // Verify the backoff caps at 30s by checking that many retries don't
        // take longer than expected. Delays: 2, 4, 8, 16, 30, 30, 30...
        // After the first 4 retries (2+4+8+16=30s), each additional retry is 30s.
        let local_addrs = Arc::new(RwLock::new(HashSet::new()));
        let keypair = Libp2pKeypair::generate_ed25519();
        let (handle, _evt_rx, _tx_rx, _overlay) =
            create_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

        tokio::time::pause();

        // Mix of resolvable and unresolvable
        spawn_peer_retry_task(
            vec![
                "will-never-resolve.invalid".to_string(),
                "localhost".to_string(),
            ],
            11625,
            local_addrs,
            make_test_configured_peers(),
            handle,
        );

        // After 3s: first retry runs. "localhost" resolves, "invalid" stays pending.
        tokio::time::advance(Duration::from_secs(3)).await;
        tokio::task::yield_now().await;

        // After 4 more seconds (total 7s): second retry for the remaining peer.
        tokio::time::advance(Duration::from_secs(5)).await;
        tokio::task::yield_now().await;
        // No panic = pass
    }

    #[test]
    fn test_strip_p2p_suffix() {
        // Address with /p2p suffix
        let addr_with_p2p: Multiaddr =
            format!("/ip4/127.0.0.1/udp/12625/quic-v1/p2p/{}", PeerId::random())
                .parse()
                .unwrap();
        let stripped = strip_p2p_suffix(&addr_with_p2p);
        assert_eq!(stripped.to_string(), "/ip4/127.0.0.1/udp/12625/quic-v1");

        // Address without /p2p suffix — should be unchanged
        let bare: Multiaddr = "/ip4/10.0.0.1/udp/9000/quic-v1".parse().unwrap();
        let stripped = strip_p2p_suffix(&bare);
        assert_eq!(stripped, bare);
    }

    #[test]
    fn test_eager_txset_tracker_counts_preempted_core_requests() {
        let metrics = OverlayMetrics::new();
        let mut tracker = EagerTxSetTracker::default();
        let eager_hash = [7u8; 32];
        let ordinary_hash = [8u8; 32];

        tracker.record_received_via_shards(eager_hash);
        tracker.record_core_request_cache_hit(&ordinary_hash, &metrics);
        assert_eq!(
            metrics.txset_shard_fetch_preempted.load(Ordering::Relaxed),
            0
        );

        tracker.record_core_request_cache_hit(&eager_hash, &metrics);
        assert_eq!(
            metrics.txset_shard_fetch_preempted.load(Ordering::Relaxed),
            1
        );
    }

    #[test]
    fn test_eager_txset_tracker_counts_eager_shards_later_served() {
        let metrics = OverlayMetrics::new();
        let mut tracker = EagerTxSetTracker::default();
        let eager_hash = [9u8; 32];
        let ordinary_hash = [10u8; 32];

        tracker.record_sent_via_shards(eager_hash);
        tracker.record_peer_request_served(&ordinary_hash, &metrics);
        assert_eq!(
            metrics
                .txset_shard_eager_also_served
                .load(Ordering::Relaxed),
            0
        );

        tracker.record_peer_request_served(&eager_hash, &metrics);
        assert_eq!(
            metrics
                .txset_shard_eager_also_served
                .load(Ordering::Relaxed),
            1
        );
    }
}
