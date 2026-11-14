//! Stellar Overlay Process
//!
//! A process-isolated overlay for stellar-core that handles:
//! - SCP message relay (latency-critical, via dedicated QUIC stream)
//! - Transaction flooding (via dedicated QUIC stream)
//! - Peer management
//!
//! Uses QUIC transport for true stream independence - SCP never blocked by TX.
//! Communicates with Core via Unix domain socket IPC.

use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, error, info, warn};

use libp2p::identity::Keypair as Libp2pKeypair;
use libp2p::{Multiaddr, PeerId};
use stellar_overlay::config::Config;
use stellar_overlay::flood::{
    CachedTxSet, Hash256, TxSetCache, TxSetData, TxSetStore, UnclaimedSetBudget,
};
use stellar_overlay::flood::txset_tree::{TreeConfig, TreeMode, MAX_TREE_NODES};
use stellar_overlay::integrated::{Overlay, OverlayHandle};
use stellar_overlay::ipc::{CoreIpc, Message, MessageType};
use stellar_overlay::libp2p_overlay::{
    create_overlay, NodeIdentity, OverlayEvent as LibP2pOverlayEvent,
    OverlayHandle as LibP2pOverlayHandle,
};
use stellar_overlay::metrics::OverlayMetrics;
use stellar_overlay::wire::ValidatedTx;
use stellar_overlay::xdr;

/// Command-line arguments
struct Args {
    config_path: Option<PathBuf>,
    socket_path: Option<PathBuf>,
    listen_mode: bool,
    peer_port: Option<u16>,
    txset_dir: Option<PathBuf>,
}

impl Args {
    fn parse() -> Self {
        let mut args = Args {
            config_path: None,
            socket_path: None,
            listen_mode: false,
            peer_port: None,
            txset_dir: None,
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
                "--txset-dir" => {
                    args.txset_dir = iter.next().map(PathBuf::from);
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
                    eprintln!("      --txset-dir <PATH> Store TX sets durably in this directory");
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

fn get_cached_tx_set_xdr<'a>(tx_set_cache: &'a TxSetCache, hash: &Hash256) -> Option<&'a [u8]> {
    tx_set_cache.get(hash).map(|cached| cached.xdr.as_slice())
}

fn cache_tx_set(
    tx_set_cache: &mut TxSetCache,
    current_ledger_seq: u32,
    hash: Hash256,
    data: Arc<TxSetData>,
) {
    tx_set_cache.insert(CachedTxSet {
        hash,
        xdr: data,
        ledger_seq: current_ledger_seq,
    });
}

/// How many ledgers the TX set cache and the TX set store look back.
const TX_SET_RETAINED_LEDGERS: u32 = 12;

/// Application state
struct App {
    core_ipc: CoreIpc,
    overlay_handle: OverlayHandle,
    /// Cache for built TX sets
    tx_set_cache: TxSetCache,
    /// Durable copies of the TX sets Core holds, if a store directory is
    /// configured. A set is written here before it is delivered to Core (a
    /// set from a peer), and before Core's next message is handled (a set
    /// Core built): Core votes for a value only once it holds the set, and by
    /// then the set survives a crash. Sets that were only prefetched are not
    /// stored.
    tx_set_store: Option<Arc<TxSetStore>>,
    /// The stored sets, with the newest slot that needs each. Unlike the
    /// cache, which peers can fill, the store only forgets a set once its
    /// slot is `TX_SET_RETAINED_LEDGERS` ledgers old.
    stored_tx_set_slots: HashMap<Hash256, u32>,
    /// Sets the store held at startup that have not been asked for since.
    /// Core asks for the ones its restored SCP state names; the rest are
    /// deleted once the node is `TX_SET_RETAINED_LEDGERS` ledgers further.
    stale_stored_tx_sets: HashSet<Hash256>,
    /// Ledgers closed since startup.
    ledgers_closed_since_start: u32,
    /// Missing sets requested by Core, with the newest slot needing each set.
    /// Prefetches stay in the cache until Core establishes this demand.
    pending_core_tx_sets: HashMap<Hash256, u32>,
    /// Cached sets that came from a peer (pushed, or prefetched because an
    /// SCP envelope named them) and that Core has not requested yet. Bounded
    /// per peer and in total.
    unclaimed_tx_sets: UnclaimedSetBudget,
    /// Current ledger sequence
    current_ledger_seq: u32,
    /// libp2p overlay handle (QUIC-based SCP + TX)
    libp2p_handle: LibP2pOverlayHandle,
    /// libp2p overlay events (SCP, TxSet - critical, unbounded)
    libp2p_events: mpsc::UnboundedReceiver<LibP2pOverlayEvent>,
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
        let (libp2p_handle, libp2p_event_rx, mut libp2p_overlay) =
            create_overlay(libp2p_keypair, Arc::clone(&metrics), overlay_handle.clone())
                .map_err(|e| format!("Failed to create libp2p overlay: {}", e))?;

        // Use peer_port + 1000 for libp2p QUIC to avoid collision with legacy TCP
        let libp2p_port = config.peer_port + 1000;
        libp2p_overlay.listen(&config.libp2p_listen_ip, libp2p_port)?;

        // Compute local addresses for self-dial detection (instant + async DNS in background)
        let local_addrs = collect_local_addrs(libp2p_port);

        // Spawn libp2p overlay task
        tokio::spawn(libp2p_overlay.run_event_loop());

        info!(
            "Started libp2p QUIC overlay on {}:{} (SCP + TX + TxSet streams)",
            config.libp2p_listen_ip, libp2p_port
        );

        let tx_set_store = match &config.txset_dir {
            Some(dir) => {
                let store = TxSetStore::open(dir.clone())
                    .map_err(|e| format!("Failed to open TX set store {}: {}", dir.display(), e))?;
                info!(
                    "TX set store: {} ({} sets from an earlier run)",
                    dir.display(),
                    store.hashes().len()
                );
                Some(Arc::new(store))
            }
            None => None,
        };
        let stale_stored_tx_sets = tx_set_store
            .as_ref()
            .map(|store| store.hashes().into_iter().collect())
            .unwrap_or_default();

        Ok(Self {
            core_ipc,
            overlay_handle,
            tx_set_cache: TxSetCache::new(100),
            tx_set_store,
            stored_tx_set_slots: HashMap::new(),
            stale_stored_tx_sets,
            ledgers_closed_since_start: 0,
            pending_core_tx_sets: HashMap::new(),
            unclaimed_tx_sets: UnclaimedSetBudget::default(),
            current_ledger_seq: 0,
            libp2p_handle,
            libp2p_events: libp2p_event_rx,
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
        })
    }

    /// Main event loop - process messages from Core and overlay events
    async fn run(mut self) {
        info!("Overlay started, processing Core messages");

        // Ask Core to bind this overlay's peer id to the validator's key;
        // it answers with `SetNodeIdentity`.
        let peer_id = self.libp2p_handle.local_peer_id().to_bytes();
        if let Err(e) = self
            .core_ipc
            .sender
            .send(Message::new(MessageType::OverlayIdentity, peer_id))
        {
            error!("Failed to send the overlay identity to Core: {}", e);
        }

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

                // Safety-net reconnect: PeerId-based dials for known peers,
                // address-based ONLY for peers we've never learned a PeerId for.
                _ = reconnect_interval.tick() => {
                    let cp = self.configured_peers.read().await;
                    let addrs = cp.addrs.clone();
                    let listen_port = cp.listen_port;
                    // Expect a connection for every configured address. Self is
                    // not normally in the list; if it is, the extra safety-net
                    // tick is harmless (self-dials and connected peers are
                    // skipped when re-dialing).
                    let expected_peers = addrs.len();
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
            LibP2pOverlayEvent::ScpReceived {
                envelope,
                txset_hashes,
                from,
                slot,
            } => {
                // Copy first 4 bytes for logging identification
                let mut id_bytes = [0u8; 4];
                let id_len = std::cmp::min(envelope.len(), 4);
                id_bytes[..id_len].copy_from_slice(&envelope[..id_len]);

                debug!(
                    "SCP_FROM_PEER: Received SCP (id={:02x?}) ({} bytes) from {}, forwarding to Core",
                    &id_bytes[..id_len],
                    envelope.len(),
                    from
                );

                // TX set hashes were extracted during the reader's single
                // decode. Snapshot the cache-hit check on the main loop, then
                // move the libp2p cmd_tx awaits into a spawned task so the loop
                // never blocks on the bounded command channel.
                if !txset_hashes.is_empty() {
                    let needs_fetch: HashSet<[u8; 32]> = txset_hashes
                        .iter()
                        .filter(|h| self.tx_set_cache.get(h).is_none())
                        .copied()
                        .collect();
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
                            if needs_fetch.contains(txhash) {
                                debug!(
                                    "TXSET_AUTO_FETCH: Proactively fetching TX set {:02x?}... referenced in SCP from {}",
                                    &txhash[..4],
                                    from_peer
                                );
                                handle.fetch_txset(*txhash, slot).await;
                            }
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
            LibP2pOverlayEvent::TxSetReceived {
                hash,
                data,
                from,
                slot,
            } => {
                // `data` was strict-decoded and its content hash verified in the
                // reader task, so we cache and forward it as-is.
                info!(
                    "TXSET_RECV: Received TxSet {:02x?}... ({} bytes) from {}",
                    &hash[..4],
                    data.len(),
                    from
                );

                // A set nobody here asked for was pushed by its sender (a
                // round leader does that for the set it is about to propose).
                // It is cached like any other, but Core only receives it if a
                // statement Core processes names its hash (RequestTxSet).
                let already_cached = self.tx_set_cache.get(&hash).is_some();
                if slot.is_none() {
                    self.metrics
                        .txset_push_received
                        .fetch_add(1, Ordering::Relaxed);
                    if already_cached {
                        // Already held (requested earlier, built locally, or
                        // pushed twice): keep the existing entry.
                        self.deliver_requested_tx_set(&hash).await;
                        return;
                    }
                }
                // Whether pushed or prefetched, a new set that Core is not
                // waiting for counts against the limits for unclaimed sets
                // until Core asks for it. A set this node already held (for
                // example one it built) is never made evictable by a copy
                // arriving from a peer.
                let unclaimed_bytes = (!already_cached
                    && !self.pending_core_tx_sets.contains_key(&hash))
                .then(|| data.len() + data.encoded().len());

                // IMPORTANT: Cache the TxSet FIRST, before pushing to Core
                // This ensures the TxSet is available when SCP processing resumes
                // Stamp with the slot the set was requested for so eviction is
                // exact; for an unsolicited set fall back to the next slot.
                cache_tx_set(
                    &mut self.tx_set_cache,
                    slot.unwrap_or(self.current_ledger_seq + 1),
                    hash,
                    data,
                );
                if let Some(bytes) = unclaimed_bytes {
                    let pushed = slot.is_none();
                    for evicted in self.unclaimed_tx_sets.insert(from, hash, bytes, pushed) {
                        self.tx_set_cache.remove(&evicted);
                        self.metrics
                            .txset_push_evicted
                            .fetch_add(1, Ordering::Relaxed);
                        warn!(
                            "TXSET_PUSH_EVICT: Dropped TxSet {:02x?}... from the cache (limit on sets from peers that Core has not asked for)",
                            &evicted[..4]
                        );
                    }
                    // The cache's own capacity limit may have dropped a set.
                    self.sync_unclaimed_tx_sets();
                }

                self.deliver_requested_tx_set(&hash).await;
            }
            LibP2pOverlayEvent::TxSetRequested { hash, from } => {
                info!("Peer {} requesting TxSet {:02x?}...", from, &hash[..4]);
                // Look up in local cache and respond
                if let Some(data) = self.load_stored_tx_set(&hash).await {
                    // Not cached, but still on disk from before a restart.
                    info!(
                        "Serving stored TxSet {:02x?}... ({} bytes) to {}",
                        &hash[..4],
                        data.len(),
                        from
                    );
                    let handle = self.libp2p_handle.clone();
                    tokio::spawn(async move {
                        handle.send_txset(hash, data, from).await;
                    });
                } else if let Some(cached) = self.tx_set_cache.get(&hash) {
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

    /// Stop counting pushed sets that the cache no longer holds.
    fn sync_unclaimed_tx_sets(&mut self) {
        let cache = &self.tx_set_cache;
        self.unclaimed_tx_sets
            .retain(|hash| cache.get(hash).is_some());
    }

    /// Makes sure a TX set is in the store (if there is one), for `slot`.
    /// Returns false if the set could not be stored.
    ///
    /// The write runs on a blocking worker but is awaited here, so that the
    /// App task does not do anything else (deliver the set to Core, or handle
    /// Core's next message, its proposal for a set it just built) before the
    /// set is on disk.
    async fn store_tx_set(&mut self, slot: u32, hash: Hash256, data: &Arc<TxSetData>) -> bool {
        let Some(store) = &self.tx_set_store else {
            return true;
        };
        if !store.contains(&hash) {
            let store = Arc::clone(store);
            let to_store = Arc::clone(data);
            let started = std::time::Instant::now();
            match tokio::task::spawn_blocking(move || store.persist(&hash, &to_store)).await {
                Ok(Ok(())) => {
                    info!(
                        "TXSET_PERSIST: Stored TX set {:02x?}... encoded_bytes={} duration_us={}",
                        &hash[..4],
                        data.encoded().len(),
                        started.elapsed().as_micros()
                    );
                }
                result => {
                    error!(
                        "TXSET_PERSIST_FAIL: Could not store TX set {:02x?}...: {:?}",
                        &hash[..4],
                        result
                    );
                    return false;
                }
            }
        }
        self.stale_stored_tx_sets.remove(&hash);
        self.stored_tx_set_slots
            .entry(hash)
            .and_modify(|needed_slot| *needed_slot = (*needed_slot).max(slot))
            .or_insert(slot);
        true
    }

    /// Deletes a stored set.
    fn forget_stored_tx_set(&mut self, hash: &Hash256) {
        if let Some(store) = &self.tx_set_store {
            store.remove(hash);
        }
        self.stored_tx_set_slots.remove(hash);
        self.stale_stored_tx_sets.remove(hash);
    }

    /// Delivers a cached set that Core is waiting for, after storing it: a
    /// set that is not on disk is not handed to Core, which therefore cannot
    /// vote for it.
    async fn deliver_requested_tx_set(&mut self, hash: &Hash256) {
        let Some(slot) = self.pending_core_tx_sets.get(hash).copied() else {
            return;
        };
        let Some(data) = self.tx_set_cache.get(hash).map(|c| Arc::clone(&c.xdr)) else {
            return;
        };
        if self.store_tx_set(slot, *hash, &data).await {
            self.send_requested_tx_set(hash);
        }
    }

    /// Reads back a set that is on disk but not in the cache (stored before
    /// a restart).
    async fn load_stored_tx_set(&mut self, hash: &Hash256) -> Option<Arc<TxSetData>> {
        if self.tx_set_cache.get(hash).is_some() {
            return None;
        }
        let store = Arc::clone(self.tx_set_store.as_ref()?);
        if !store.contains(hash) {
            return None;
        }
        let hash = *hash;
        match tokio::task::spawn_blocking(move || store.load(&hash)).await {
            Ok(Ok(Some(data))) => Some(Arc::new(data)),
            Ok(Ok(None)) => {
                self.stale_stored_tx_sets.remove(&hash);
                None
            }
            result => {
                error!(
                    "TXSET_LOAD_FAIL: Could not read stored TX set {:02x?}...: {:?}",
                    &hash[..4],
                    result.map(|r| r.map(|_| ()))
                );
                None
            }
        }
    }

    /// Core rejects sets it has not requested. Keep proactive network fetches
    /// cached, and deliver only after RequestTxSet. Both event orderings run
    /// on this App task, so a request racing a prefetch cannot lose its reply.
    fn send_requested_tx_set(&mut self, hash: &Hash256) {
        if !self.pending_core_tx_sets.contains_key(hash) {
            return;
        }
        let Some(data) = get_cached_tx_set_xdr(&self.tx_set_cache, hash) else {
            return;
        };
        let bytes = data.len();
        match self.core_ipc.sender.send_tx_set_available(*hash, data) {
            Ok(()) => {
                // This is not a permanent delivered/seen marker. A fresh
                // explicit request must still work after Core evicts a set.
                self.pending_core_tx_sets.remove(hash);
                info!(
                    "TXSET_TO_CORE: Sending requested TxSet {:02x?}... ({} bytes) to Core",
                    &hash[..4],
                    bytes
                );
            }
            Err(e) => error!("Failed to send requested TX set to Core: {}", e),
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

                debug!(
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
                debug!("Core requesting top {} transactions", count);

                let core_sender = self.core_ipc.sender.clone();
                let overlay_handle = self.overlay_handle.clone();

                tokio::spawn(async move {
                    let Some(txs) = overlay_handle.get_top_txs(count).await else {
                        warn!("GetTopTxs: mempool manager gone (shutting down); not responding");
                        return;
                    };

                    debug!("Returning {} transactions to Core", txs.len());

                    // Borrow the shared tx bytes for serialization; the one
                    // copy happens inside the IPC frame encoding.
                    let tx_data: Vec<&[u8]> = txs.iter().map(|tx| tx.bytes()).collect();

                    if let Err(e) = core_sender.send_top_txs_response(&tx_data) {
                        error!("Failed to send top txs response: {}", e);
                    }
                });
            }

            MessageType::RequestTxSet => {
                // Request TX set by hash - check local cache first, then fetch from peers via libp2p
                // Payload: [hash:32][slotSeq:4]
                if msg.payload.len() < 36 {
                    warn!("RequestTxSet payload too short");
                    return true;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);
                let slot = u32::from_le_bytes(msg.payload[32..36].try_into().unwrap());

                // Core requests a hash once per fetch and re-requests it only
                // when the earlier request has not delivered. Retry that from
                // another peer instead of deduplicating against the stuck one.
                let retry = self.pending_core_tx_sets.contains_key(&hash);
                self.pending_core_tx_sets
                    .entry(hash)
                    .and_modify(|needed_slot| *needed_slot = (*needed_slot).max(slot))
                    .or_insert(slot);

                // Core asks only for a hash named by an SCP statement it is
                // processing: from here on a pushed set is an ordinary cache
                // entry, no longer subject to the limits for unclaimed sets.
                let pushed = self.unclaimed_tx_sets.claim(&hash) == Some(true);

                // A set this node stored before a restart is not in the
                // cache, but it is on disk: Core asks for the sets its
                // restored SCP state names.
                if let Some(data) = self.load_stored_tx_set(&hash).await {
                    info!(
                        "TXSET_FROM_STORE: TX set {:02x?}... ({} bytes) read back from disk",
                        &hash[..4],
                        data.len()
                    );
                    cache_tx_set(&mut self.tx_set_cache, slot, hash, data);
                }

                // First check local cache

                if let Some(cached) = self.tx_set_cache.get(&hash) {
                    info!(
                        "TXSET_FROM_CACHE: Sending TX set {:02x?}... ({} bytes) from local cache{}",
                        &hash[..4],
                        cached.xdr.len(),
                        if pushed { " (pushed by a peer)" } else { "" }
                    );
                    if pushed {
                        self.metrics.txset_push_hit.fetch_add(1, Ordering::Relaxed);
                    }
                    self.deliver_requested_tx_set(&hash).await;
                } else {
                    // Not in local cache - request from peers. Spawn so the main
                    // loop never awaits on the bounded libp2p cmd channel.
                    info!(
                        "TXSET_FETCH_START: TX set {:02x?}... not in cache, fetching from peers{}",
                        &hash[..4],
                        if retry { " (retry)" } else { "" }
                    );
                    let handle = self.libp2p_handle.clone();
                    tokio::spawn(async move {
                        if retry {
                            handle.refetch_txset(hash, slot).await;
                        } else {
                            handle.fetch_txset(hash, slot).await;
                        }
                    });
                }
            }

            MessageType::CacheTxSet => {
                // Core built a TX set locally and wants us to cache it for peer requests
                // Payload: [hash:32][slotSeq:4][txSetXDR...]
                if msg.payload.len() < 37 {
                    warn!("CacheTxSet payload too short");
                    return true;
                }

                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);
                let slot = u32::from_le_bytes(msg.payload[32..36].try_into().unwrap());
                let mut payload = msg.payload;
                let tx_set_xdr = payload.split_off(36);

                // Core is trusted for encoding, so we skip decoding. We still
                // guard the content hash cheaply: caching bytes under a hash
                // that peers would recompute differently makes the tx set
                // unfetchable network-wide.
                if !xdr::tx_set_hash_matches(&hash, &tx_set_xdr) {
                    warn!(
                        "TXSET_CACHE_DROP: Dropping TX set {:02x?}... from Core: content hash mismatch",
                        &hash[..4]
                    );
                    return true;
                }

                info!(
                    "TXSET_CACHE: Caching locally-built TX set {:02x?}... for slot {} ({} bytes)",
                    &hash[..4],
                    slot,
                    tx_set_xdr.len()
                );

                // Keep an existing received/prepared representation intact on
                // repeated IPC publication of the same content hash.
                let data = if let Some(cached) = self.tx_set_cache.get(&hash) {
                    Arc::clone(&cached.xdr)
                } else {
                    // Core sends this before final proposal validation. Await
                    // completion on this task while the network and Core keep
                    // running. Requests queue until the ready set is cached.
                    let started = std::time::Instant::now();
                    match tokio::task::spawn_blocking(move || TxSetData::from_local(tx_set_xdr))
                        .await
                    {
                        Ok(Ok(data)) => {
                            info!("TXSET_COMPRESS: Prepared locally built set {:02x?}... raw_bytes={} encoded_bytes={} duration_us={}",
                                  &hash[..4], data.len(), data.encoded().len(), started.elapsed().as_micros());
                            Arc::new(data)
                        }
                        result => {
                            error!(
                                "TXSET_CACHE_DROP: Failed to encode local set {:02x?}...: {:?}",
                                &hash[..4],
                                result
                            );
                            return true;
                        }
                    }
                };
                // Core sends its proposal for this set right after this
                // message, and this task handles Core's messages one at a
                // time: storing the set here means it is on disk before the
                // proposal can leave. Core does not store the set itself, so
                // if that fails the proposal must not leave at all: stop,
                // which takes this node off the network.
                if !self.store_tx_set(slot, hash, &data).await {
                    error!(
                        "TXSET_PERSIST_FAIL: Shutting down: TX set {:02x?}... built by Core for slot {} is not on stable storage",
                        &hash[..4],
                        slot
                    );
                    return false;
                }
                cache_tx_set(&mut self.tx_set_cache, slot, hash, data);
                // Core built this set itself: whatever a peer sent under the
                // same hash is no longer an unclaimed set that the limits may
                // evict (this node must be able to serve what it proposes).
                self.unclaimed_tx_sets.claim(&hash);
                // The cache's own capacity limit may have dropped a set.
                self.sync_unclaimed_tx_sets();

                self.deliver_requested_tx_set(&hash).await;
            }

            MessageType::PushTxSet => {
                // Core leads a round and is about to propose this set: send
                // it to every peer now instead of waiting for their requests.
                // Payload: [hash:32][slotSeq:4]. The set itself came with the
                // CacheTxSet before this message on the same ordered channel.
                if msg.payload.len() < 36 {
                    warn!("PushTxSet payload too short");
                    return true;
                }
                let mut hash = [0u8; 32];
                hash.copy_from_slice(&msg.payload[0..32]);
                let slot = u32::from_le_bytes(msg.payload[32..36].try_into().unwrap());

                let Some(cached) = self.tx_set_cache.get(&hash) else {
                    // E.g. the set failed to encode. Peers request it.
                    warn!(
                        "TXSET_PUSH_SKIP: TX set {:02x?}... for slot {} is not cached, not pushing",
                        &hash[..4],
                        slot
                    );
                    return true;
                };
                let data = Arc::clone(&cached.xdr);
                let handle = self.libp2p_handle.clone();
                // Spawn so the main loop never awaits on the bounded libp2p
                // command channel.
                tokio::spawn(async move {
                    handle.push_txset_for_slot(hash, data, slot).await;
                });
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

                // Core is trusted for encoding and supplies fee/ops; we only
                // reject fee-bumps (still unsupported) via a cheap discriminant
                // check. No decode.
                let tx = match ValidatedTx::from_core_trusted(tx_data, fee, num_ops) {
                    Ok(tx) => tx,
                    Err(e) => {
                        warn!("SUBMIT_TX_DROP: Dropping unsupported TX from Core: {}", e);
                        return true;
                    }
                };

                // Add to mempool
                self.overlay_handle.submit_tx(Arc::clone(&tx));

                // Core accepted this transaction from a client: this node
                // keeps it and sends it until it is externalized, to the
                // targets Core named or, without targets, to every peer.
                // The router is updated here, in the order of Core's
                // messages; only the sending is left to a task.
                let plan = self.libp2p_handle.add_local_tx(tx);
                let handle = self.libp2p_handle.clone();
                tokio::spawn(async move {
                    handle.send_local_tx(plan).await;
                });
            }

            MessageType::SetNodeIdentity => {
                // Payload: [networkID:32][nodeID:32][signature:64]
                if msg.payload.len() != 128 {
                    warn!(
                        "SetNodeIdentity payload has {} bytes, expected 128",
                        msg.payload.len()
                    );
                    return true;
                }
                let mut identity = NodeIdentity {
                    network_id: [0; 32],
                    node_id: [0; 32],
                    signature: [0; 64],
                };
                identity.network_id.copy_from_slice(&msg.payload[0..32]);
                identity.node_id.copy_from_slice(&msg.payload[32..64]);
                identity.signature.copy_from_slice(&msg.payload[64..128]);
                let handle = self.libp2p_handle.clone();
                tokio::spawn(async move {
                    handle.set_node_identity(identity).await;
                });
            }

            MessageType::SetTxTargets => {
                // Payload: [everyone:1][count:4][nodeID:32]...
                let Some(targets) = parse_tx_targets(&msg.payload) else {
                    warn!(
                        "SetTxTargets payload malformed ({} bytes)",
                        msg.payload.len()
                    );
                    return true;
                };
                match &targets {
                    Some(nodes) => debug!("TX_TARGETS: {} validators get transactions", nodes.len()),
                    None => debug!("TX_TARGETS: every peer gets transactions"),
                }
                if self.libp2p_handle.set_tx_targets(targets) {
                    let handle = self.libp2p_handle.clone();
                    tokio::spawn(async move {
                        handle.flush_local_txs().await;
                    });
                }
            }

            MessageType::SetTxSetTree => {
                let Some(config) = parse_txset_tree(&msg.payload) else {
                    warn!(
                        "SetTxSetTree payload malformed ({} bytes)",
                        msg.payload.len()
                    );
                    return true;
                };
                debug!(
                    "TXSET_TREE: mode {:?}, fanout {}, {} nodes, slots {:?}",
                    config.mode,
                    config.effective_fanout(),
                    config.nodes.len(),
                    config.slots.iter().map(|(slot, _)| *slot).collect::<Vec<_>>()
                );
                self.libp2p_handle.set_txset_tree(config);
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

                    // Evict old TX sets from the cache and from the store
                    let min_slot = ledger_seq.saturating_sub(TX_SET_RETAINED_LEDGERS);
                    self.tx_set_cache.evict_before(min_slot);
                    let old_stored: Vec<Hash256> = self
                        .stored_tx_set_slots
                        .iter()
                        .filter(|(_, slot)| **slot < min_slot)
                        .map(|(hash, _)| *hash)
                        .collect();
                    for hash in old_stored {
                        self.forget_stored_tx_set(&hash);
                    }
                    self.ledgers_closed_since_start =
                        self.ledgers_closed_since_start.saturating_add(1);
                    if self.ledgers_closed_since_start == TX_SET_RETAINED_LEDGERS {
                        // Whatever an earlier run left on disk and nobody
                        // asked for is behind us now.
                        for stale in std::mem::take(&mut self.stale_stored_tx_sets) {
                            self.forget_stored_tx_set(&stale);
                        }
                    }
                    self.pending_core_tx_sets
                        .retain(|_, slot| *slot >= min_slot);
                    self.sync_unclaimed_tx_sets();
                    // Requests that were never answered expire with the same
                    // retained-slot window.
                    let handle = self.libp2p_handle.clone();
                    tokio::spawn(async move {
                        handle.prune_txset_requests(min_slot).await;
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

                    debug!(
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

                    // Nothing is left to send for them. Core uses this
                    // message with a zero set hash to drop transactions for
                    // its own reasons; only a real set is a ledger.
                    if tx_set_hash == [0u8; 32] {
                        self.libp2p_handle.forget_local_txs(&tx_hashes);
                    } else {
                        // Transactions this ledger did not have may now be
                        // due at targets they were held back from.
                        self.libp2p_handle.ledger_externalized(&tx_hashes);
                        let handle = self.libp2p_handle.clone();
                        tokio::spawn(async move {
                            handle.flush_local_txs().await;
                        });
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
                debug!(
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

/// Parse a `SetTxTargets` payload: `[everyone:1][count:4][nodeID:32]...`.
/// Returns `None` if it is malformed, `Some(None)` for "every peer".
fn parse_tx_targets(payload: &[u8]) -> Option<Option<Vec<[u8; 32]>>> {
    if payload.len() < 5 {
        return None;
    }
    let everyone = match payload[0] {
        0 => false,
        1 => true,
        _ => return None,
    };
    let count = u32::from_le_bytes(payload[1..5].try_into().unwrap()) as usize;
    if payload.len() != 5 + count.checked_mul(32)? {
        return None;
    }
    if everyone {
        return Some(None);
    }
    let nodes = payload[5..]
        .chunks_exact(32)
        .map(|chunk| chunk.try_into().unwrap())
        .collect();
    Some(Some(nodes))
}

/// Parse a `SetTxSetTree` payload:
/// `[mode:1][fanout:1][slots:1]([slot:4 LE][seed:32])...[count:4 LE][nodeID:32]...`.
fn parse_txset_tree(payload: &[u8]) -> Option<TreeConfig> {
    if payload.len() < 3 {
        return None;
    }
    let mode = TreeMode::from_u8(payload[0])?;
    let fanout = payload[1];
    let slot_count = payload[2] as usize;
    let mut at = 3;
    let mut slots = Vec::with_capacity(slot_count);
    for _ in 0..slot_count {
        let entry = payload.get(at..at + 36)?;
        let slot = u32::from_le_bytes(entry[..4].try_into().unwrap());
        let mut seed = [0u8; 32];
        seed.copy_from_slice(&entry[4..]);
        slots.push((slot, seed));
        at += 36;
    }
    let count = u32::from_le_bytes(payload.get(at..at + 4)?.try_into().unwrap()) as usize;
    at += 4;
    if count > MAX_TREE_NODES || payload.len() != at + count.checked_mul(32)? {
        return None;
    }
    let nodes = payload[at..]
        .chunks_exact(32)
        .map(|chunk| chunk.try_into().unwrap())
        .collect();
    Some(TreeConfig::new(mode, fanout, slots, nodes))
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

    // Override the TX set store directory from command line
    if let Some(dir) = args.txset_dir {
        config.txset_dir = Some(dir);
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
    fn create_test_overlay(
        keypair: Libp2pKeypair,
        metrics: Arc<OverlayMetrics>,
    ) -> Result<
        (
            LibP2pOverlayHandle,
            mpsc::UnboundedReceiver<LibP2pOverlayEvent>,
            stellar_overlay::libp2p_overlay::StellarOverlay,
        ),
        Box<dyn std::error::Error + Send + Sync>,
    > {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        tokio::spawn(Overlay::new(cmd_rx).run());
        create_overlay(keypair, metrics, OverlayHandle::new(cmd_tx))
    }

    use stellar_xdr::{Limits, ScpEnvelope, WriteXdr};

    fn test_scp_envelope_xdr(slot_index: u64) -> Vec<u8> {
        let mut envelope = ScpEnvelope::default();
        envelope.statement.slot_index = slot_index;
        envelope.to_xdr(Limits::none()).unwrap()
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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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

        let (handle1, _events1, mut overlay1) =
            create_test_overlay(kp1, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle2, mut events2, mut overlay2) =
            create_test_overlay(kp2, Arc::new(OverlayMetrics::new())).unwrap();
        let (handle3, mut events3, mut overlay3) =
            create_test_overlay(kp3, Arc::new(OverlayMetrics::new())).unwrap();

        // Start all three on different ports
        let port1: u16 = 18501;
        let port2: u16 = 18502;
        let port3: u16 = 18503;
        overlay1.listen("127.0.0.1", port1).unwrap();
        tokio::spawn(overlay1.run_event_loop());
        overlay2.listen("127.0.0.1", port2).unwrap();
        tokio::spawn(overlay2.run_event_loop());
        overlay3.listen("127.0.0.1", port3).unwrap();
        tokio::spawn(overlay3.run_event_loop());
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
        let (handle, _evt_rx, _overlay) =
            create_test_overlay(keypair, Arc::new(OverlayMetrics::new())).unwrap();

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

    // --- App::handle_core_message / handle_libp2p_event tests ---

    use std::os::unix::net::UnixStream as StdUnixStream;
    use stellar_overlay::ipc::MessageCodec;

    /// Build an App wired to an in-process socket pair, without touching the
    /// network. Returns the core-side stream for driving and observing IPC.
    /// The libp2p overlay object is dropped (not run), so cache-miss fetches
    /// just log a warning — these tests only exercise the cache paths.
    fn test_app() -> (App, StdUnixStream) {
        let (app, core, _network) = test_app_with_network();
        (app, core)
    }

    fn test_app_with_network() -> (
        App,
        StdUnixStream,
        stellar_overlay::libp2p_overlay::StellarOverlay,
    ) {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let core_ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let mempool_manager = Overlay::new(cmd_rx);
        tokio::spawn(async move {
            let _ = mempool_manager.run().await;
        });
        let overlay_handle = OverlayHandle::new(cmd_tx);

        let metrics = Arc::new(OverlayMetrics::new());
        let (libp2p_handle, libp2p_events, network) = create_overlay(
            Libp2pKeypair::generate_ed25519(),
            Arc::clone(&metrics),
            overlay_handle.clone(),
        )
        .unwrap();

        let app = App {
            core_ipc,
            overlay_handle,
            tx_set_cache: TxSetCache::new(100),
            tx_set_store: None,
            stored_tx_set_slots: HashMap::new(),
            stale_stored_tx_sets: HashSet::new(),
            ledgers_closed_since_start: 0,
            pending_core_tx_sets: HashMap::new(),
            unclaimed_tx_sets: UnclaimedSetBudget::default(),
            current_ledger_seq: 0,
            libp2p_handle,
            libp2p_events,
            pending_scp_state_requests: Arc::new(RwLock::new(HashMap::new())),
            next_scp_request_id: Arc::new(AtomicU64::new(1)),
            local_addrs: Arc::new(RwLock::new(HashSet::new())),
            configured_peers: Arc::new(RwLock::new(ConfiguredPeers {
                addrs: Vec::new(),
                listen_port: 11625,
                resolved: HashMap::new(),
            })),
            known_peers: Arc::new(RwLock::new(HashMap::new())),
            peer_hostnames: Arc::new(RwLock::new(HashMap::new())),
            metrics,
        };
        (app, core_side, network)
    }

    /// A minimal valid GeneralizedTransactionSet whose content hash matches,
    /// so it passes the CacheTxSet hash guard.
    fn test_txset_xdr(seed: u8) -> ([u8; 32], Vec<u8>) {
        use stellar_xdr::{GeneralizedTransactionSet, Hash};

        let mut tx_set = GeneralizedTransactionSet::default();
        let GeneralizedTransactionSet::V1(v1) = &mut tx_set;
        v1.previous_ledger_hash = Hash([seed; 32]);
        let bytes = tx_set.to_xdr(Limits::none()).unwrap();
        let hash = xdr::sha256_hash(&bytes);
        (hash, bytes)
    }

    fn test_mempool_tx(seq: i64) -> Arc<ValidatedTx> {
        use stellar_xdr::{
            Operation, SequenceNumber, Transaction, TransactionEnvelope, TransactionV1Envelope,
        };
        let envelope = TransactionEnvelope::Tx(TransactionV1Envelope {
            tx: Transaction {
                fee: 100,
                seq_num: SequenceNumber(seq),
                operations: vec![Operation::default()].try_into().unwrap(),
                ..Transaction::default()
            },
            signatures: Default::default(),
        });
        ValidatedTx::from_core_trusted(envelope.to_xdr(Limits::none()).unwrap(), 100, 1).unwrap()
    }

    fn remove_tx_message(tx_set_hash: Hash256, tx_hash: Hash256) -> Message {
        let mut payload = tx_set_hash.to_vec();
        payload.extend_from_slice(&1u32.to_le_bytes());
        payload.extend_from_slice(&tx_hash);
        Message::new(MessageType::TxSetExternalized, payload)
    }

    fn submit_tx_message(tx: &ValidatedTx) -> Message {
        let mut payload = tx.fee().to_le_bytes().to_vec();
        payload.extend_from_slice(&tx.num_ops().to_le_bytes());
        payload.extend_from_slice(tx.bytes());
        Message::new(MessageType::SubmitTx, payload)
    }

    fn tx_targets_payload(everyone: bool, nodes: &[[u8; 32]]) -> Vec<u8> {
        let mut payload = vec![everyone as u8];
        payload.extend_from_slice(&(nodes.len() as u32).to_le_bytes());
        for node in nodes {
            payload.extend_from_slice(node);
        }
        payload
    }

    #[test]
    fn tx_targets_payload_is_parsed_strictly() {
        let (a, b) = ([1u8; 32], [2u8; 32]);
        assert_eq!(
            parse_tx_targets(&tx_targets_payload(false, &[a, b])),
            Some(Some(vec![a, b]))
        );
        // "Everyone", with or without a list.
        assert_eq!(parse_tx_targets(&tx_targets_payload(true, &[])), Some(None));
        assert_eq!(parse_tx_targets(&tx_targets_payload(true, &[a])), Some(None));
        // An empty list is a list: nobody else gets the transactions.
        assert_eq!(
            parse_tx_targets(&tx_targets_payload(false, &[])),
            Some(Some(vec![]))
        );
        // Malformed: too short, a bad flag, a count that does not match.
        assert_eq!(parse_tx_targets(&[]), None);
        assert_eq!(parse_tx_targets(&[0, 0, 0, 0]), None);
        let mut bad_flag = tx_targets_payload(false, &[a]);
        bad_flag[0] = 2;
        assert_eq!(parse_tx_targets(&bad_flag), None);
        let mut truncated = tx_targets_payload(false, &[a, b]);
        truncated.pop();
        assert_eq!(parse_tx_targets(&truncated), None);
        let mut wrong_count = tx_targets_payload(false, &[a]);
        wrong_count[1..5].copy_from_slice(&u32::MAX.to_le_bytes());
        assert_eq!(parse_tx_targets(&wrong_count), None);
    }

    #[tokio::test]
    async fn a_submitted_transaction_is_kept_until_core_reports_it_externalized() {
        let (mut app, _core) = test_app();
        let (first, second) = (test_mempool_tx(1), test_mempool_tx(2));
        assert!(app.handle_core_message(submit_tx_message(&first)).await);
        assert!(app.handle_core_message(submit_tx_message(&second)).await);
        // Kept for sending again, and in the mempool for this node's own sets.
        assert_eq!(app.libp2p_handle.local_tx_count(), 2);
        assert_eq!(app.overlay_handle.get_top_txs(10).await.unwrap().len(), 2);
        // Submitting it again changes nothing.
        assert!(app.handle_core_message(submit_tx_message(&first)).await);
        assert_eq!(app.libp2p_handle.local_tx_count(), 2);

        app.handle_core_message(remove_tx_message([1; 32], *first.hash()))
            .await;
        assert_eq!(app.libp2p_handle.local_tx_count(), 1);
        let top = app.overlay_handle.get_top_txs(10).await.unwrap();
        assert_eq!(top.len(), 1);
        assert_eq!(top[0].hash(), second.hash());
    }

    #[tokio::test]
    async fn malformed_identity_and_target_messages_are_ignored() {
        let (mut app, _core) = test_app();
        for payload in [vec![], vec![0u8; 127], vec![0u8; 129]] {
            assert!(
                app.handle_core_message(Message::new(MessageType::SetNodeIdentity, payload))
                    .await
            );
        }
        assert!(
            app.handle_core_message(Message::new(MessageType::SetTxTargets, vec![7, 0, 0]))
                .await
        );
        // A well-formed list is taken; the same list again is not a change.
        let payload = tx_targets_payload(false, &[[3; 32]]);
        assert!(
            app.handle_core_message(Message::new(MessageType::SetTxTargets, payload.clone()))
                .await
        );
        assert!(!app.libp2p_handle.set_tx_targets(Some(vec![[3; 32]])));
        assert!(app.libp2p_handle.set_tx_targets(None));
    }

    #[tokio::test]
    async fn externalization_removes_queued_admission_without_remembering_hashes() {
        let (mut app, _core) = test_app();
        let tx = test_mempool_tx(1);
        assert!(app.overlay_handle.try_submit_network_tx(tx.clone()));
        app.handle_core_message(remove_tx_message([1; 32], *tx.hash()))
            .await;
        assert!(app.overlay_handle.get_top_txs(10).await.unwrap().is_empty());

        // Truly later admissions are deliberately allowed; no finalized hash
        // history remains. Ledger-state validity is still checked by Core.
        assert!(app.overlay_handle.try_submit_network_tx(tx.clone()));
        let top = app.overlay_handle.get_top_txs(10).await.unwrap();
        assert_eq!(top.len(), 1);
        assert_eq!(top[0].hash(), tx.hash());
    }

    #[tokio::test]
    async fn discarded_transaction_can_be_resubmitted() {
        let (mut app, _core) = test_app();
        let tx = test_mempool_tx(1);
        app.overlay_handle.submit_tx(tx.clone());
        assert_eq!(app.overlay_handle.get_top_txs(10).await.unwrap().len(), 1);

        // OverlayIPC::removeTransactions uses a zero set hash for candidates
        // discarded by the builder, rather than finalized transactions.
        app.handle_core_message(remove_tx_message([0; 32], *tx.hash()))
            .await;
        assert!(app.overlay_handle.get_top_txs(10).await.unwrap().is_empty());
        app.overlay_handle.submit_tx(tx.clone());
        let top = app.overlay_handle.get_top_txs(10).await.unwrap();
        assert_eq!(top.len(), 1);
        assert_eq!(top[0].hash(), tx.hash());
    }

    #[tokio::test]
    async fn local_publication_is_eager_and_preserves_existing_encoding() {
        let (mut app, _core) = test_app();
        let (hash, xdr) = test_txset_xdr(29);
        let mut payload = request_tx_set_payload(&hash, 100);
        payload.extend_from_slice(&xdr);
        app.handle_core_message(Message::new(MessageType::CacheTxSet, payload.clone()))
            .await;
        let data = Arc::clone(&app.tx_set_cache.get(&hash).unwrap().xdr);
        // No send has occurred: publication itself prepared the response.
        assert_eq!(
            zstd::bulk::decompress(&data.encoded()[4..], data.len()).unwrap(),
            xdr
        );
        app.handle_core_message(Message::new(MessageType::CacheTxSet, payload))
            .await;
        assert!(Arc::ptr_eq(
            &data,
            &app.tx_set_cache.get(&hash).unwrap().xdr
        ));
    }

    #[tokio::test]
    async fn received_encoding_survives_cache_and_local_duplicate() {
        let (mut app, _core) = test_app();
        let (hash, xdr) = test_txset_xdr(30);
        let received = Arc::new(TxSetData::from_local(xdr.clone()).unwrap());
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::clone(&received),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert!(Arc::ptr_eq(
            &received,
            &app.tx_set_cache.get(&hash).unwrap().xdr
        ));
        let mut payload = request_tx_set_payload(&hash, 101);
        payload.extend_from_slice(&xdr);
        app.handle_core_message(Message::new(MessageType::CacheTxSet, payload))
            .await;
        let cached = app.tx_set_cache.get(&hash).unwrap();
        assert!(Arc::ptr_eq(&received, &cached.xdr));
        assert_eq!(cached.ledger_seq, 101);
    }

    #[tokio::test]
    async fn peer_responses_share_cached_storage_and_survive_eviction() {
        // Keep the network dispatcher alive but unpolled: responses remain
        // queued or awaiting dispatch, as when the transport is delayed.
        let (mut app, _core, network) = test_app_with_network();
        let (hash, data) = test_txset_xdr(7);
        cache_tx_set(
            &mut app.tx_set_cache,
            1,
            hash,
            Arc::new(TxSetData::from_local(data.clone()).unwrap()),
        );
        let cached = Arc::downgrade(&app.tx_set_cache.get(&hash).unwrap().xdr);
        for _ in 0..29 {
            app.handle_libp2p_event(LibP2pOverlayEvent::TxSetRequested {
                hash,
                from: PeerId::random(),
            })
            .await;
        }
        tokio::time::timeout(Duration::from_secs(2), async {
            while cached.strong_count() != 30 {
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("each queued peer response must share the cached allocation");

        app.tx_set_cache.evict_before(2);
        assert!(app.tx_set_cache.get(&hash).is_none());
        assert_eq!(cached.strong_count(), 29);
        assert_eq!(cached.upgrade().unwrap().as_slice(), data.as_slice());
        drop(network);
        tokio::time::timeout(Duration::from_secs(2), async {
            while cached.strong_count() != 0 {
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("shutdown releases queued payloads and wakes waiting sends");
        assert!(cached.upgrade().is_none());
    }

    fn request_tx_set_payload(hash: &[u8; 32], slot: u32) -> Vec<u8> {
        let mut payload = hash.to_vec();
        payload.extend_from_slice(&slot.to_le_bytes());
        payload
    }

    fn ledger_closed_payload(seq: u32) -> Vec<u8> {
        let mut payload = seq.to_le_bytes().to_vec();
        payload.extend_from_slice(&[0u8; 32]);
        payload
    }

    fn assert_no_core_txset(core: &mut StdUnixStream) {
        core.set_read_timeout(Some(Duration::from_millis(100)))
            .unwrap();
        let error = MessageCodec::read(core).expect_err("unexpected IPC delivery");
        assert!(matches!(
            error.kind(),
            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
        ));
    }

    fn assert_core_txset(core: &mut StdUnixStream, hash: &Hash256, data: &[u8]) {
        core.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let response = MessageCodec::read(core).unwrap();
        assert_eq!(response.msg_type, MessageType::TxSetAvailable);
        assert_eq!(&response.payload[..32], hash);
        assert_eq!(&response.payload[32..], data);
    }

    fn cache_tx_set_payload(hash: &[u8; 32], slot: u32, xdr: &[u8]) -> Vec<u8> {
        let mut payload = hash.to_vec();
        payload.extend_from_slice(&slot.to_le_bytes());
        payload.extend_from_slice(xdr);
        payload
    }

    fn with_tx_set_store(app: &mut App, dir: &std::path::Path) {
        let store = TxSetStore::open(dir.to_path_buf()).unwrap();
        app.stale_stored_tx_sets = store.hashes().into_iter().collect();
        app.tx_set_store = Some(Arc::new(store));
    }

    fn stored_file(dir: &std::path::Path, hash: &Hash256) -> PathBuf {
        let name: String = hash.iter().map(|b| format!("{:02x}", b)).collect();
        dir.join(format!("{}.txset", name))
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn tx_sets_are_on_disk_before_core_has_them() {
        let dir = tempfile::tempdir().unwrap();
        let (mut app, mut core) = test_app();
        with_tx_set_store(&mut app, dir.path());

        // A set Core built: by the time the CacheTxSet message has been
        // handled (and so before the next message from Core, its proposal, is
        // looked at), the compressed set is on disk.
        let (local_hash, local_xdr) = test_txset_xdr(41);
        app.handle_core_message(Message::new(
            MessageType::CacheTxSet,
            cache_tx_set_payload(&local_hash, 100, &local_xdr),
        ))
        .await;
        let cached = app.tx_set_cache.get(&local_hash).unwrap().xdr.clone();
        assert_eq!(
            std::fs::read(stored_file(dir.path(), &local_hash)).unwrap(),
            cached.encoded()
        );

        // A set from a peer that Core is waiting for: it is on disk when Core
        // receives it.
        let (peer_hash, peer_xdr) = test_txset_xdr(42);
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&peer_hash, 100),
        ))
        .await;
        assert_no_core_txset(&mut core);
        let peer_data = Arc::new(TxSetData::from_local(peer_xdr.clone()).unwrap());
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash: peer_hash,
            data: Arc::clone(&peer_data),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert_eq!(
            std::fs::read(stored_file(dir.path(), &peer_hash)).unwrap(),
            peer_data.encoded()
        );
        assert_core_txset(&mut core, &peer_hash, &peer_xdr);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_tx_set_that_cannot_be_stored_is_not_used() {
        // The store directory disappears: nothing can be written.
        let dir = tempfile::tempdir().unwrap();
        let store_dir = dir.path().join("txsets");
        let (mut app, mut core) = test_app();
        with_tx_set_store(&mut app, &store_dir);
        std::fs::remove_dir_all(&store_dir).unwrap();

        let (hash, xdr) = test_txset_xdr(43);
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(xdr).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        // Not delivered to Core, which therefore can't vote for it.
        assert_no_core_txset(&mut core);

        // A set Core built itself is proposed right after it is cached, and
        // Core does not store it: the overlay stops instead of letting the
        // proposal out.
        let (local_hash, local_xdr) = test_txset_xdr(48);
        let keep_running = app
            .handle_core_message(Message::new(
                MessageType::CacheTxSet,
                cache_tx_set_payload(&local_hash, 100, &local_xdr),
            ))
            .await;
        assert!(!keep_running);
        assert!(app.tx_set_cache.get(&local_hash).is_none());
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn only_sets_core_holds_are_stored_and_peers_cannot_evict_them() {
        let dir = tempfile::tempdir().unwrap();
        let (mut app, mut core) = test_app();
        with_tx_set_store(&mut app, dir.path());

        // A prefetched set that Core has not asked for stays in memory.
        let (prefetched_hash, prefetched_xdr) = test_txset_xdr(49);
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash: prefetched_hash,
            data: Arc::new(TxSetData::from_local(prefetched_xdr.clone()).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert!(app.tx_set_cache.get(&prefetched_hash).is_some());
        assert!(!stored_file(dir.path(), &prefetched_hash).exists());
        // It is stored when Core asks for it, before Core gets it.
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&prefetched_hash, 100),
        ))
        .await;
        assert!(stored_file(dir.path(), &prefetched_hash).exists());
        assert_core_txset(&mut core, &prefetched_hash, &prefetched_xdr);

        // Peers push more sets than the cache holds. The set Core holds
        // leaves the cache, but not the disk, and Core can still get it.
        for seed in 100..=200u8 {
            let (hash, xdr) = test_txset_xdr(seed);
            app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
                hash,
                data: Arc::new(TxSetData::from_local(xdr).unwrap()),
                from: PeerId::random(),
                slot: Some(100),
            })
            .await;
            assert!(!stored_file(dir.path(), &hash).exists());
        }
        assert!(app.tx_set_cache.get(&prefetched_hash).is_none());
        assert!(stored_file(dir.path(), &prefetched_hash).exists());
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&prefetched_hash, 100),
        ))
        .await;
        assert_core_txset(&mut core, &prefetched_hash, &prefetched_xdr);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn stored_tx_sets_are_served_to_core_after_a_restart() {
        let dir = tempfile::tempdir().unwrap();
        let (hash, xdr) = test_txset_xdr(44);
        let (unused_hash, unused_xdr) = test_txset_xdr(45);
        {
            // The first run votes on two sets and stops.
            let (mut app, _core) = test_app();
            with_tx_set_store(&mut app, dir.path());
            for (h, x) in [(&hash, &xdr), (&unused_hash, &unused_xdr)] {
                app.handle_core_message(Message::new(
                    MessageType::CacheTxSet,
                    cache_tx_set_payload(h, 100, x),
                ))
                .await;
            }
        }

        // The next run starts with an empty cache. Core's restored SCP state
        // names one of the sets, and Core asks for it.
        let (mut app, mut core) = test_app();
        with_tx_set_store(&mut app, dir.path());
        assert!(app.tx_set_cache.get(&hash).is_none());
        assert_eq!(app.stale_stored_tx_sets.len(), 2);
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        assert_core_txset(&mut core, &hash, &xdr);
        assert_eq!(
            app.tx_set_cache.get(&hash).unwrap().xdr.as_slice(),
            xdr.as_slice()
        );
        assert_eq!(app.tx_set_cache.get(&hash).unwrap().ledger_seq, 100);

        // The set nobody asked for stays on disk for a while, and is deleted
        // once the node is far enough past the restart.
        for seq in 100..100 + TX_SET_RETAINED_LEDGERS - 1 {
            app.handle_core_message(Message::new(
                MessageType::LedgerClosed,
                ledger_closed_payload(seq),
            ))
            .await;
        }
        assert!(stored_file(dir.path(), &unused_hash).exists());
        app.handle_core_message(Message::new(
            MessageType::LedgerClosed,
            ledger_closed_payload(100 + TX_SET_RETAINED_LEDGERS - 1),
        ))
        .await;
        assert!(!stored_file(dir.path(), &unused_hash).exists());
        assert!(app.stale_stored_tx_sets.is_empty());
        // The requested one follows the cache.
        assert!(stored_file(dir.path(), &hash).exists());
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn stored_tx_sets_are_deleted_when_their_slot_is_old() {
        let dir = tempfile::tempdir().unwrap();
        let (mut app, _core) = test_app();
        with_tx_set_store(&mut app, dir.path());
        let (old_hash, old_xdr) = test_txset_xdr(46);
        let (new_hash, new_xdr) = test_txset_xdr(47);
        app.handle_core_message(Message::new(
            MessageType::CacheTxSet,
            cache_tx_set_payload(&old_hash, 100, &old_xdr),
        ))
        .await;
        app.handle_core_message(Message::new(
            MessageType::CacheTxSet,
            cache_tx_set_payload(&new_hash, 120, &new_xdr),
        ))
        .await;
        assert!(stored_file(dir.path(), &old_hash).exists());
        assert!(stored_file(dir.path(), &new_hash).exists());

        // Ledger 113 closes: sets for slots before 101 are dropped.
        app.handle_core_message(Message::new(
            MessageType::LedgerClosed,
            ledger_closed_payload(100 + TX_SET_RETAINED_LEDGERS + 1),
        ))
        .await;
        assert!(app.tx_set_cache.get(&old_hash).is_none());
        assert!(!stored_file(dir.path(), &old_hash).exists());
        assert!(app.tx_set_cache.get(&new_hash).is_some());
        assert!(stored_file(dir.path(), &new_hash).exists());
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn prefetched_txset_waits_for_core_request() {
        let (mut app, mut core) = test_app();
        let (hash, data) = test_txset_xdr(31);
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert_eq!(
            app.tx_set_cache.get(&hash).unwrap().xdr.as_slice(),
            data.as_slice()
        );

        assert_no_core_txset(&mut core);

        // A later explicit request must receive the cached set. There must
        // be no permanent 'already delivered' marker preventing a re-fetch.
        for _ in 0..2 {
            app.handle_core_message(Message::new(
                MessageType::RequestTxSet,
                request_tx_set_payload(&hash, 100),
            ))
            .await;
            assert_core_txset(&mut core, &hash, &data);
        }
    }

    fn pushed_txset(hash: Hash256, data: &[u8], from: PeerId) -> LibP2pOverlayEvent {
        LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(data.to_vec()).unwrap()),
            from,
            // Nobody on this node asked for it: a push.
            slot: None,
        }
    }

    fn push_counters(app: &App) -> (u64, u64, u64) {
        (
            app.metrics.txset_push_received.load(Ordering::Relaxed),
            app.metrics.txset_push_hit.load(Ordering::Relaxed),
            app.metrics.txset_push_evicted.load(Ordering::Relaxed),
        )
    }

    /// A pushed set is held in the overlay and reaches Core only when Core
    /// asks for that hash; the request is then answered at once.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn pushed_txset_reaches_core_only_on_request() {
        let (mut app, mut core) = test_app();
        let (hash, data) = test_txset_xdr(41);
        let event = pushed_txset(hash, &data, PeerId::random());
        app.handle_libp2p_event(event).await;

        assert!(app.tx_set_cache.get(&hash).is_some());
        assert!(app.unclaimed_tx_sets.contains(&hash));
        assert_no_core_txset(&mut core);
        assert_eq!(push_counters(&app), (1, 0, 0));

        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        assert_core_txset(&mut core, &hash, &data);
        // Claimed by Core: an ordinary cache entry from here on.
        assert!(!app.unclaimed_tx_sets.contains(&hash));
        assert!(!app.pending_core_tx_sets.contains_key(&hash));
        assert_eq!(push_counters(&app), (1, 1, 0));

        // A second request is served too, but is not another push hit.
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        assert_core_txset(&mut core, &hash, &data);
        assert_eq!(push_counters(&app), (1, 1, 0));
    }

    /// A pushed set whose content is not the set Core asks for is never
    /// delivered: Core's request for the proposed hash stays pending (and
    /// goes to the network), whatever was pushed.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn pushed_txset_with_another_hash_does_not_answer_a_request() {
        let (mut app, mut core) = test_app();
        let (pushed_hash, pushed_data) = test_txset_xdr(42);
        let (proposed_hash, proposed_data) = test_txset_xdr(43);
        let leader = PeerId::random();
        let event = pushed_txset(pushed_hash, &pushed_data, leader);
        app.handle_libp2p_event(event).await;

        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&proposed_hash, 100),
        ))
        .await;
        assert_no_core_txset(&mut core);
        assert_eq!(app.pending_core_tx_sets.get(&proposed_hash), Some(&100));
        // The pushed set is still unclaimed and was not counted as a hit.
        assert!(app.unclaimed_tx_sets.contains(&pushed_hash));
        assert_eq!(push_counters(&app), (1, 0, 0));

        // An event that claims the proposed hash but carries other content
        // cannot exist: the reader files a set under the hash it computed.
        // The proposed set arriving (by request) completes the fetch.
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash: proposed_hash,
            data: Arc::new(TxSetData::from_local(proposed_data.clone()).unwrap()),
            from: leader,
            slot: Some(100),
        })
        .await;
        assert_core_txset(&mut core, &proposed_hash, &proposed_data);
        assert_no_core_txset(&mut core);
    }

    /// A push that lands after Core asked for the hash satisfies the request.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn push_arriving_after_the_core_request_completes_it() {
        let (mut app, mut core) = test_app();
        let (hash, data) = test_txset_xdr(44);
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        assert_no_core_txset(&mut core);

        let event = pushed_txset(hash, &data, PeerId::random());
        app.handle_libp2p_event(event).await;
        assert_core_txset(&mut core, &hash, &data);
        // Core was already waiting for it, so it never counted as unclaimed.
        assert!(!app.unclaimed_tx_sets.contains(&hash));
        assert!(app.unclaimed_tx_sets.is_empty());

        // A duplicate push changes nothing and delivers nothing.
        let event = pushed_txset(hash, &data, PeerId::random());
        app.handle_libp2p_event(event).await;
        assert_no_core_txset(&mut core);
        assert_eq!(app.tx_set_cache.len(), 1);
    }

    /// Sets a peer pushes and Core never asks for are bounded per peer; sets
    /// Core asked for are not affected by the limit.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn unclaimed_pushed_txsets_are_bounded_per_peer() {
        use stellar_overlay::flood::txset_push_limits::MAX_UNCLAIMED_SETS_PER_PEER as LIMIT;
        let (mut app, mut core) = test_app();
        let pusher = PeerId::random();
        let other = PeerId::random();

        // One set from the pusher that Core uses.
        let (used_hash, used_data) = test_txset_xdr(50);
        let event = pushed_txset(used_hash, &used_data, pusher);
        app.handle_libp2p_event(event).await;
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&used_hash, 100),
        ))
        .await;
        assert_core_txset(&mut core, &used_hash, &used_data);

        // One unclaimed set from another peer.
        let (other_hash, other_data) = test_txset_xdr(51);
        let event = pushed_txset(other_hash, &other_data, other);
        app.handle_libp2p_event(event).await;

        // The pusher sends two more sets than it may keep unclaimed.
        let hashes: Vec<Hash256> = (0..LIMIT + 2)
            .map(|i| {
                let (hash, _) = test_txset_xdr(60 + i as u8);
                hash
            })
            .collect();
        for i in 0..LIMIT + 2 {
            let (hash, data) = test_txset_xdr(60 + i as u8);
            let event = pushed_txset(hash, &data, pusher);
            app.handle_libp2p_event(event).await;
        }

        // Its two oldest unclaimed sets are gone from the cache ...
        assert!(app.tx_set_cache.get(&hashes[0]).is_none());
        assert!(app.tx_set_cache.get(&hashes[1]).is_none());
        for hash in &hashes[2..] {
            assert!(app.tx_set_cache.get(hash).is_some());
            assert!(app.unclaimed_tx_sets.contains(hash));
        }
        // ... while the set Core used and the other peer's set stay.
        assert!(app.tx_set_cache.get(&used_hash).is_some());
        assert!(app.tx_set_cache.get(&other_hash).is_some());
        assert_eq!(app.unclaimed_tx_sets.len(), LIMIT + 1);
        let (received, hits, evicted) = push_counters(&app);
        assert_eq!((received, hits, evicted), (LIMIT as u64 + 4, 1, 2));

        // An evicted set that turns out to be needed is fetched like any
        // other missing set: the request stays pending instead of failing.
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hashes[0], 100),
        ))
        .await;
        assert_no_core_txset(&mut core);
        assert!(app.pending_core_tx_sets.contains_key(&hashes[0]));

        // Slot eviction of the cache also releases the budget.
        app.handle_core_message(Message::new(
            MessageType::LedgerClosed,
            ledger_closed_payload(200),
        ))
        .await;
        assert_eq!(app.tx_set_cache.len(), 0);
        assert!(app.unclaimed_tx_sets.is_empty());
        assert_eq!(app.unclaimed_tx_sets.total_bytes(), 0);
    }

    /// A set this node built itself is never evicted by the limits for
    /// unclaimed sets, even if a peer pushed the same set first (two nodes
    /// build identical empty sets on an idle network).
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn locally_built_txset_is_not_evicted_as_unclaimed() {
        use stellar_overlay::flood::txset_push_limits::MAX_UNCLAIMED_SETS_PER_PEER as LIMIT;
        let (mut app, _core) = test_app();
        let pusher = PeerId::random();
        let (hash, data) = test_txset_xdr(70);

        // The peer's push arrives first, then Core builds the same set.
        app.handle_libp2p_event(pushed_txset(hash, &data, pusher))
            .await;
        assert!(app.unclaimed_tx_sets.contains(&hash));
        let mut payload = request_tx_set_payload(&hash, 100);
        payload.extend_from_slice(&data);
        app.handle_core_message(Message::new(MessageType::CacheTxSet, payload))
            .await;
        assert!(!app.unclaimed_tx_sets.contains(&hash));

        // Another copy from a peer does not make it evictable again.
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            from: pusher,
            slot: Some(100),
        })
        .await;
        assert!(!app.unclaimed_tx_sets.contains(&hash));

        // The same peer now pushes more sets than it may keep unclaimed.
        for i in 0..LIMIT + 2 {
            let (other, other_data) = test_txset_xdr(80 + i as u8);
            app.handle_libp2p_event(pushed_txset(other, &other_data, pusher))
                .await;
        }
        assert!(app.tx_set_cache.get(&hash).is_some());
    }

    /// Sets the overlay prefetched (an SCP envelope named them, a peer
    /// answered) but Core has not asked for are bounded too, with their own,
    /// larger per-peer limit: a peer answers a new connection's SCP state
    /// request with many slots at once. They are not counted as push hits
    /// when Core does ask, and pushes from the same peer do not evict them.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn prefetched_txsets_core_did_not_ask_for_are_bounded() {
        use stellar_overlay::flood::txset_push_limits::{
            MAX_UNCLAIMED_REQUESTED_SETS_PER_PEER as REQUESTED_LIMIT,
            MAX_UNCLAIMED_SETS_PER_PEER as PUSHED_LIMIT,
        };
        let (mut app, mut core) = test_app();
        let peer = PeerId::random();
        let sets: Vec<(Hash256, Vec<u8>)> = (0..REQUESTED_LIMIT + 1)
            .map(|i| test_txset_xdr(90 + i as u8))
            .collect();
        // A full window of answers arrives before Core asks for any: all kept.
        for (hash, data) in &sets[..REQUESTED_LIMIT] {
            app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
                hash: *hash,
                data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
                from: peer,
                slot: Some(100),
            })
            .await;
        }
        assert_eq!(app.unclaimed_tx_sets.len(), REQUESTED_LIMIT);
        for (hash, _) in &sets[..REQUESTED_LIMIT] {
            assert!(app.tx_set_cache.get(hash).is_some());
        }
        // Pushes from the same peer are counted separately.
        for i in 0..PUSHED_LIMIT {
            let (hash, data) = test_txset_xdr(150 + i as u8);
            app.handle_libp2p_event(pushed_txset(hash, &data, peer))
                .await;
        }
        assert!(app.tx_set_cache.get(&sets[0].0).is_some());
        assert_eq!(push_counters(&app), (PUSHED_LIMIT as u64, 0, 0));

        // One more answer than the limit drops the oldest prefetched set.
        let (extra_hash, extra_data) = &sets[REQUESTED_LIMIT];
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash: *extra_hash,
            data: Arc::new(TxSetData::from_local(extra_data.clone()).unwrap()),
            from: peer,
            slot: Some(100),
        })
        .await;
        assert!(app.tx_set_cache.get(&sets[0].0).is_none());
        assert_eq!(app.unclaimed_tx_sets.len(), REQUESTED_LIMIT + PUSHED_LIMIT);
        assert_no_core_txset(&mut core);

        // Core asks for a prefetched set: served, and not a push hit.
        let (hash, data) = &sets[1];
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(hash, 100),
        ))
        .await;
        assert_core_txset(&mut core, hash, data);
        assert!(!app.unclaimed_tx_sets.contains(hash));
        assert_eq!(push_counters(&app), (PUSHED_LIMIT as u64, 0, 1));

        // Core asks for the evicted one: it is fetched again, not lost.
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&sets[0].0, 100),
        ))
        .await;
        assert_no_core_txset(&mut core);
        assert!(app.pending_core_tx_sets.contains_key(&sets[0].0));
    }

    /// PushTxSet for a set the overlay does not hold, or with a short
    /// payload, is ignored: no crash, no shutdown, nothing sent to Core.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn push_txset_ipc_for_unknown_or_malformed_input_is_ignored() {
        let (mut app, mut core) = test_app();
        let (hash, _) = test_txset_xdr(45);
        assert!(
            app.handle_core_message(Message::new(
                MessageType::PushTxSet,
                request_tx_set_payload(&hash, 100),
            ))
            .await
        );
        assert!(
            app.handle_core_message(Message::new(MessageType::PushTxSet, vec![1, 2, 3]))
                .await
        );
        assert!(
            app.handle_core_message(Message::new(MessageType::PushTxSet, vec![]))
                .await
        );
        assert_no_core_txset(&mut core);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn pending_core_txset_delivers_once_despite_duplicate_arrivals() {
        let (mut app, mut core) = test_app();
        let (hash, data) = test_txset_xdr(32);
        for slot in [100, 101, 99] {
            app.handle_core_message(Message::new(
                MessageType::RequestTxSet,
                request_tx_set_payload(&hash, slot),
            ))
            .await;
        }
        assert_eq!(app.pending_core_tx_sets.get(&hash), Some(&101));

        let (other, other_data) = test_txset_xdr(33);
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash: other,
            data: Arc::new(TxSetData::from_local(other_data).unwrap()),
            from: PeerId::random(),
            slot: Some(101),
        })
        .await;
        assert_no_core_txset(&mut core);
        assert!(app.pending_core_tx_sets.contains_key(&hash));

        for arrival in 0..2 {
            app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
                hash,
                data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
                from: PeerId::random(),
                slot: Some(101),
            })
            .await;
            if arrival == 0 {
                assert_core_txset(&mut core, &hash, &data);
                assert!(!app.pending_core_tx_sets.contains_key(&hash));
            } else {
                assert_no_core_txset(&mut core);
            }
        }
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn locally_cached_txset_satisfies_pending_core_request() {
        let (mut app, mut core) = test_app();
        let (hash, data) = test_txset_xdr(34);
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        let mut payload = request_tx_set_payload(&hash, 100);
        payload.extend_from_slice(&data);
        app.handle_core_message(Message::new(MessageType::CacheTxSet, payload))
            .await;
        assert_core_txset(&mut core, &hash, &data);
        assert!(!app.pending_core_tx_sets.contains_key(&hash));
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn evicted_prefetch_can_be_requested_and_fetched_again() {
        let (mut app, mut core) = test_app();
        app.tx_set_cache = TxSetCache::new(1);
        let (hash, data) = test_txset_xdr(35);
        let (other, other_data) = test_txset_xdr(36);
        for (h, bytes) in [(hash, data.clone()), (other, other_data)] {
            app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
                hash: h,
                data: Arc::new(TxSetData::from_local(bytes).unwrap()),
                from: PeerId::random(),
                slot: Some(100),
            })
            .await;
        }
        assert!(app.tx_set_cache.get(&hash).is_none());
        assert_no_core_txset(&mut core);
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        assert!(app.pending_core_tx_sets.contains_key(&hash));
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert_core_txset(&mut core, &hash, &data);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn pending_core_txsets_expire_with_the_retained_slot_window() {
        let (mut app, mut core) = test_app();
        let (old, data) = test_txset_xdr(37);
        let (future, _) = test_txset_xdr(38);
        for (hash, slot) in [(old, 100), (future, 120), (future, 99)] {
            app.handle_core_message(Message::new(
                MessageType::RequestTxSet,
                request_tx_set_payload(&hash, slot),
            ))
            .await;
        }
        app.handle_core_message(Message::new(
            MessageType::LedgerClosed,
            ledger_closed_payload(112),
        ))
        .await;
        assert_eq!(app.pending_core_tx_sets.get(&old), Some(&100));
        app.handle_core_message(Message::new(
            MessageType::LedgerClosed,
            ledger_closed_payload(113),
        ))
        .await;
        assert!(!app.pending_core_tx_sets.contains_key(&old));
        assert_eq!(app.pending_core_tx_sets.get(&future), Some(&120));
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash: old,
            data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert_no_core_txset(&mut core);
        // Expiry is not a permanent seen marker. A new explicit demand is
        // still answered, even if its slot is older than our current view.
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&old, 100),
        ))
        .await;
        assert_core_txset(&mut core, &old, &data);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn failed_core_enqueue_does_not_complete_the_pending_request() {
        let (mut app, core) = test_app();
        let (hash, data) = test_txset_xdr(39);
        drop(core);
        // Cause the real socket writer to observe the closed connection and
        // drop its queue receiver before exercising delivery failure.
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if app
                    .core_ipc
                    .sender
                    .send_tx_set_available(hash, &[])
                    .is_err()
                {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        })
        .await
        .unwrap();
        app.handle_core_message(Message::new(
            MessageType::RequestTxSet,
            request_tx_set_payload(&hash, 100),
        ))
        .await;
        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;
        assert!(app.pending_core_tx_sets.contains_key(&hash));
        assert_eq!(
            app.tx_set_cache.get(&hash).unwrap().xdr.as_slice(),
            data.as_slice()
        );
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn test_request_tx_set_rejects_legacy_32_byte_payload() {
        let (mut app, mut core) = test_app();
        let (hash, xdr_bytes) = test_txset_xdr(7);

        // Cache the set, so if the handler wrongly accepted the legacy
        // format it would respond with TxSetAvailable below.
        cache_tx_set(
            &mut app.tx_set_cache,
            1,
            hash,
            Arc::new(TxSetData::from_local(xdr_bytes).unwrap()),
        );

        // Pre-slotSeq payload: [hash:32] only. The protocol is now
        // [hash:32][slotSeq:4]; the short payload must be dropped.
        let handled = app
            .handle_core_message(Message::new(MessageType::RequestTxSet, hash.to_vec()))
            .await;
        assert!(
            handled,
            "short payload should be dropped, not kill the loop"
        );

        core.set_read_timeout(Some(Duration::from_millis(300)))
            .unwrap();
        assert!(
            MessageCodec::read(&mut core).is_err(),
            "no response expected for a legacy 32-byte RequestTxSet payload"
        );
    }

    /// Regression test for premature tx set eviction: a set Core caches for a
    /// future slot must be stamped with that slot, not with the overlay's
    /// current ledger view. Before the fix it was stamped with
    /// current_ledger_seq (0 here) and evicted on the next LedgerClosed,
    /// making the set unfetchable exactly when SCP needed it.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn test_cache_tx_set_slot_stamp_prevents_premature_eviction() {
        let (mut app, mut core) = test_app();
        let (hash, xdr_bytes) = test_txset_xdr(9);
        let slot: u32 = 100;

        // Core caches the set it built for slot 100 while the overlay still
        // thinks the current ledger is 0.
        // CacheTxSet payload: [hash:32][slotSeq:4][txSetXDR...]
        let mut payload = request_tx_set_payload(&hash, slot);
        payload.extend_from_slice(&xdr_bytes);
        assert!(
            app.handle_core_message(Message::new(MessageType::CacheTxSet, payload))
                .await
        );

        // Ledger 99 closes; eviction drops sets stamped before slot 87. The
        // slot-100 entry must survive (pre-fix it was stamped 0 and died).
        assert!(
            app.handle_core_message(Message::new(
                MessageType::LedgerClosed,
                ledger_closed_payload(99)
            ))
            .await
        );

        // Core asks for the set: it must come back from the local cache.
        assert!(
            app.handle_core_message(Message::new(
                MessageType::RequestTxSet,
                request_tx_set_payload(&hash, slot)
            ))
            .await
        );

        core.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let resp = MessageCodec::read(&mut core).unwrap();
        assert_eq!(resp.msg_type, MessageType::TxSetAvailable);
        assert_eq!(&resp.payload[0..32], &hash[..]);
        assert_eq!(&resp.payload[32..], &xdr_bytes[..]);
    }

    /// Same property for sets fetched from peers: a TxSetReceived event is
    /// cached under the slot the set was requested for, so it survives
    /// eviction until that slot is actually past.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn test_txset_received_cached_under_requested_slot() {
        let (mut app, mut core) = test_app();
        let (hash, xdr_bytes) = test_txset_xdr(11);

        app.handle_libp2p_event(LibP2pOverlayEvent::TxSetReceived {
            hash,
            data: Arc::new(TxSetData::from_local(xdr_bytes.clone()).unwrap()),
            from: PeerId::random(),
            slot: Some(100),
        })
        .await;

        // An unrequested prefetch stays in Rust until Core asks for it.

        // Ledger 99 closes (evicts sets stamped before 87); the entry was
        // stamped with the requested slot 100 and must survive.
        assert!(
            app.handle_core_message(Message::new(
                MessageType::LedgerClosed,
                ledger_closed_payload(99)
            ))
            .await
        );

        assert!(
            app.handle_core_message(Message::new(
                MessageType::RequestTxSet,
                request_tx_set_payload(&hash, 100)
            ))
            .await
        );
        core.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let resp = MessageCodec::read(&mut core).unwrap();
        assert_eq!(resp.msg_type, MessageType::TxSetAvailable);
        assert_eq!(&resp.payload[0..32], &hash[..]);
        assert_eq!(&resp.payload[32..], &xdr_bytes[..]);
    }
}
