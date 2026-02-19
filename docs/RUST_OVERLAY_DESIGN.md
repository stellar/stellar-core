# Rust Overlay Design

**Status**: Prototype — 178 Rust tests + 20+ C++ integration tests passing

---

## Overview

The Rust overlay is a **separate process** that handles all peer-to-peer
networking for stellar-core. It communicates with the C++ core via Unix
domain socket IPC.

**Key properties:**
- **Process isolation**: overlay crash doesn't crash core
- **Stream independence**: SCP consensus messages are never blocked by
  transaction traffic (separate QUIC streams)
- **Pull-based TX dissemination**: INV/GETDATA protocol prevents
  unnecessary data transfer

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                       stellar-core (C++)                         │
│                                                                  │
│  ┌────────────────┐    ┌──────────────────────────────────────┐  │
│  │  HerderImpl    │───▶│  RustOverlayManager                  │  │
│  │  (SCP logic)   │    │   └─ OverlayIPC (Unix socket client) │  │
│  └────────────────┘    └──────────────────────────────────────┘  │
│                                      │                           │
└──────────────────────────────────────│───────────────────────────┘
                                       │ Unix domain socket IPC
                                       │ (length-prefixed binary msgs)
┌──────────────────────────────────────│───────────────────────────┐
│                     stellar-overlay (Rust)                        │
│                                      │                           │
│  ┌───────────────────────────────────▼──────────────────────────┐│
│  │                    Core IPC Handler                          ││
│  │  • Reads/writes IPC messages (ipc/)                          ││
│  │  • Routes commands to libp2p overlay and mempool             ││
│  └──────────┬──────────────────────────────────┬────────────────┘│
│             │                                  │                 │
│  ┌──────────▼──────────┐          ┌────────────▼───────────────┐│
│  │  Mempool + Flood    │          │  libp2p Swarm (QUIC)       ││
│  │  (integrated.rs,    │          │                            ││
│  │   flood/)           │          │  Behaviours:               ││
│  │                     │          │  • libp2p-stream (3 protos)││
│  │  • Fee-ordered pool │          │  • Kademlia DHT            ││
│  │  • INV batching     │          │  • Identify                ││
│  │  • GETDATA tracking │          │                            ││
│  │  • TX set cache     │          │  Transport: QUIC over UDP  ││
│  └─────────────────────┘          └────────────────────────────┘│
│                                           │                      │
│                           ┌───────────────┼───────────────┐      │
│                           ▼               ▼               ▼      │
│                      [Peer 1]        [Peer 2]        [Peer N]   │
│                      SCP stream      SCP stream      SCP stream │
│                      TX stream       TX stream       TX stream  │
│                      TxSet stream    TxSet stream    TxSet str  │
└──────────────────────────────────────────────────────────────────┘
```

---

## Transport: QUIC

QUIC (via libp2p) provides:
- **Encryption**: TLS 1.3 built-in
- **Multiplexing**: Independent streams per connection
- **Stream independence**: Packet loss on TX stream doesn't block SCP
- **0-RTT**: Fast reconnection to known peers

| Parameter | Value |
|-----------|-------|
| Listen port | `peer_port + 1000` (UDP) |
| Idle connection timeout | 300s |
| Keep-alive interval | 15s |
| Max idle timeout | 60s |

libp2p address format: `/ip4/<addr>/udp/<port>/quic-v1`

---

## Stream Protocols

Three dedicated stream protocols per peer, using `libp2p-stream`:

| Protocol | Purpose | Priority | Framing |
|----------|---------|----------|---------|
| `/stellar/scp/1.0.0` | SCP consensus | Critical | 4-byte BE length prefix |
| `/stellar/tx/1.0.0` | TX dissemination (INV/GETDATA) | Normal | 1-byte type + payload |
| `/stellar/txset/1.0.0` | TX set fetch | Critical | 4-byte BE length prefix |

**SCP stream**: Push-based. Full envelopes sent immediately. A 4-byte
message on this stream is interpreted as an SCP state request (ledger
sequence number).

**TX stream**: Pull-based INV/GETDATA protocol (see [Flooding Protocol](#flooding-protocol)).

**TxSet stream**: Request-response. 32-byte hash request → `[hash:32][xdr_data...]` response.

---

## Peer Discovery: Kademlia DHT

- Bootstrap nodes configured via `known_peers` in config
- Nodes join DHT and discover peers automatically
- Kademlia mode forced to **Server** (required for peer discovery in
  localhost test networks where external address confirmation never happens)
- Bootstrap triggered on `SetPeerConfig` after 2s delay
- Identify protocol (`/stellar/1.0.0`) exchanges peer info and feeds
  addresses into Kademlia

---

## IPC Protocol

### Transport
- Unix domain socket
- Message format: `[type:u32 native-endian][length:u32 native-endian][payload]`
- Max payload: 16 MB

### Core → Overlay Messages

| Type | ID | Payload | Purpose |
|------|-----|---------|---------|
| BroadcastScp | 1 | `[scp_envelope]` | Broadcast SCP envelope to all peers |
| GetTopTxs | 2 | `[count:4]` | Request top N TXs by fee |
| RequestScpState | 3 | `[ledger_seq:4]` | Ask peers for SCP state |
| LedgerClosed | 4 | `[ledger_seq:4][ledger_hash:32]` | Notify ledger state change |
| TxSetExternalized | 5 | `[txset_hash:32][num_hashes:4][tx_hash:32]...` | TX set applied |
| ScpStateResponse | 6 | `[count:4][env_len:4][env]...` | SCP state for requesting peer |
| Shutdown | 7 | (empty) | Graceful shutdown |
| SetPeerConfig | 8 | `JSON` | Configure bootstrap peer addresses |
| SubmitTx | 10 | `[fee:i64 LE][num_ops:u32 LE][tx_envelope]` | Submit TX for flooding |
| RequestTxSet | 11 | `[hash:32]` | Request TX set by hash |
| CacheTxSet | 12 | `[hash:32][txset_xdr]` | Cache locally-built TX set |

### Overlay → Core Messages

| Type | ID | Payload | Purpose |
|------|-----|---------|---------|
| ScpReceived | 100 | `[scp_envelope]` | SCP envelope from network |
| TopTxsResponse | 101 | `[count:4][len:4][tx]...` | Response to GetTopTxs |
| PeerRequestsScpState | 102 | `[ledger_seq:4]` | Peer wants our SCP state |
| TxSetAvailable | 103 | `[hash:32][txset_xdr]` | Fetched TX set data |
| QuorumSetAvailable | 104 | `[...]` | Quorum set from peer |

---

## Flooding Protocol

### SCP Flooding (Push)

SCP envelopes are flooded immediately via push:
1. Hash envelope with Blake2b (32 bytes)
2. Check `scp_seen` LRU cache (10,000 entries) — skip if duplicate
3. Add to `scp_seen`
4. Forward to all connected peers not in `scp_sent_to` tracking cache
5. On receive: emit `ScpReceived` to core via IPC, then flood to
   remaining peers

### TX Flooding (Pull — INV/GETDATA)

Transactions use a pull-based protocol on the TX stream:

```
Sender                                    Receiver
  │                                          │
  │  INV_BATCH [hash1+fee, hash2+fee, ...]   │
  │─────────────────────────────────────────▶│
  │                                          │ (check which TXs are new)
  │           GETDATA [hash1, hash2]         │
  │◀─────────────────────────────────────────│
  │                                          │
  │  TX [full tx data for hash1]             │
  │─────────────────────────────────────────▶│
  │  TX [full tx data for hash2]             │
  │─────────────────────────────────────────▶│
```

**TX stream wire format** (1-byte type prefix):

| Type | Byte | Format |
|------|------|--------|
| TX | `0x01` | `[tx_data]` — full transaction |
| INV_BATCH | `0x02` | `[count:4 BE][entries...]` — each entry: `[hash:32][fee_per_op:i64 BE]` (40 bytes) |
| GETDATA | `0x03` | `[count:4 BE][hashes...]` — each hash: 32 bytes |

**INV batching** (`inv_batcher.rs`):
- Batches per-peer, deduplicates within batch
- Flushes at **1,000 entries** or **100ms** timeout, whichever comes first

**GETDATA tracking** (`pending_requests.rs`):
- Per-peer timeout: **1 second** (retry with different peer)
- Total timeout: **30 seconds** (give up)
- Round-robin peer selection via `inv_tracker.rs`

**TX buffer** (`tx_buffer.rs`):
- Stores TXs for responding to GETDATA requests
- Capacity: **10,000 TXs** (LRU eviction)
- Max age: **60 seconds**

**Inventory tracker** (`inv_tracker.rs`):
- Tracks which peers have advertised which TX hashes
- Capacity: **100,000 entries** (LRU eviction)
- Round-robin source selection for GETDATA

---

## Mempool

Fee-ordered transaction mempool in `flood/mempool.rs`:

- **Capacity**: 100,000 TXs (hardcoded)
- **Max age**: 300 seconds
- **Fee ordering**: `fee_per_op = fee / num_ops` via cross-multiplication
  (`fee1 * ops2` vs `fee2 * ops1`) to avoid division
- **Deduplication**: by SHA256 TX hash
- **Eviction**: lowest fee-per-op evicted when at capacity

Data structures:
- `by_hash: HashMap<TxHash, TxEntry>` — O(1) lookup
- `by_fee: BTreeSet<FeePriority>` — O(log n) ordered access
- `by_account: HashMap<AccountId, Vec<TxHash>>` — per-account grouping

---

## TX Set Building

TX set building and caching in `flood/txset.rs`:

- Builds `GeneralizedTransactionSet` v1 XDR with CLASSIC phase
- Hashes with SHA256
- Cache capacity: configurable, HashMap-based storage

---

## Shared State

All async tasks share state via `Arc<SharedState>`:

| Field | Type | Size | Purpose |
|-------|------|------|---------|
| `peer_streams` | `RwLock<HashMap<PeerId, PeerOutboundStreams>>` | — | Per-peer SCP/TX/TxSet streams |
| `scp_seen` | `RwLock<LruCache>` | 10,000 | SCP dedup (Blake2b hash) |
| `tx_seen` | `RwLock<LruCache>` | 100,000 | TX dedup (Blake2b hash) |
| `scp_sent_to` | `RwLock<LruCache>` | 10,000 | SCP flood tracking |
| `tx_sent_to` | `RwLock<LruCache>` | 100,000 | TX flood tracking (legacy) |
| `txset_sources` | `RwLock<LruCache>` | 1,000 | TX set source peer tracking |
| `inv_batcher` | `RwLock<InvBatcher>` | — | INV batching per peer |
| `inv_tracker` | `RwLock<InvTracker>` | 100,000 | Peer→TX advertisement tracking |
| `pending_getdata` | `RwLock<PendingRequests>` | — | GETDATA timeout tracking |
| `tx_buffer` | `RwLock<TxBuffer>` | 10,000 | TX data for GETDATA responses |

---

## Configuration

TOML config file (`config.rs`):

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `core_socket` | String | `/tmp/stellar-overlay.sock` | IPC socket path |
| `listen_addr` | String | `0.0.0.0:11625` | Overlay listen address |
| `libp2p_listen_ip` | String | `0.0.0.0` | QUIC bind IP |
| `peer_port` | u16 | 11625 | Base port (QUIC uses +1000) |
| `target_outbound_peers` | usize | 8 | Outbound connection target |
| `max_inbound_peers` | usize | 64 | Max inbound connections |
| `known_peers` | Vec | `[]` | Bootstrap peer addresses |
| `preferred_peers` | Vec | `[]` | Preferred peer addresses |
| `tx_push_peer_count` | usize | 8 | INV broadcast peer count |
| `max_mempool_size` | usize | 100,000 | Max TXs in mempool |
| `http_addr` | String | `127.0.0.1:11626` | HTTP endpoint |
| `log_level` | String | `info` | Log verbosity |

---

## Code Structure

```
overlay/
├── Cargo.toml
├── src/
│   ├── main.rs              # Entry point, CLI args, event loop
│   ├── lib.rs               # Public module exports
│   ├── config.rs            # TOML configuration parsing
│   ├── libp2p_overlay.rs    # libp2p swarm, stream handling, flooding
│   ├── integrated.rs        # Mempool manager, high-level overlay API
│   ├── ipc/
│   │   ├── mod.rs
│   │   ├── messages.rs      # IPC message types and codec
│   │   └── transport.rs     # Unix socket read/write
│   ├── flood/
│   │   ├── mod.rs
│   │   ├── mempool.rs       # Fee-ordered TX mempool
│   │   ├── txset.rs         # TX set building/caching
│   │   ├── inv_messages.rs  # INV/GETDATA wire format
│   │   ├── inv_batcher.rs   # Per-peer INV batching
│   │   ├── inv_tracker.rs   # Peer→TX advertisement tracking
│   │   ├── pending_requests.rs  # GETDATA timeout/retry
│   │   └── tx_buffer.rs     # TX storage for GETDATA responses
│   └── http/
│       └── mod.rs           # HTTP server (TX submission, status)
└── tests/
    ├── e2e_binary.rs        # Binary integration tests
    └── kademlia_test.rs     # Kademlia DHT tests
```

---

## Test Coverage

### Rust Tests (178 tests)

| Module | Count | What's tested |
|--------|-------|---------------|
| config | 13 | Default config, TOML parsing, validation |
| mempool | 19 | Insert, evict, dedup, fee ordering, stress (10K TXs) |
| txset | 12 | Building, hashing, caching, eviction |
| ipc/messages | 11 | Serialization roundtrip, error handling |
| ipc/transport | 12 | Unix socket send/receive, connection lifecycle |
| integrated | 11 | SubmitTx, GetTopTxs, fee ordering, CacheTxSet |
| libp2p_overlay | 32 | Multi-node SCP/TX, stream independence, flooding |
| inv_messages | 18 | Wire format encode/decode for INV/GETDATA/TX |
| inv_batcher | 11 | Batching, flush, per-peer dedup, timeout |
| inv_tracker | 12 | Source tracking, round-robin, LRU eviction |
| pending_requests | 10 | Timeout, retry, peer removal |
| tx_buffer | 10 | Insert, fetch, expiry, LRU eviction |
| kademlia_test | 4 | Multi-node DHT discovery, bootstrap |
| e2e_binary | 3 | Binary launch, IPC communication |

### C++ Integration Tests

| Test | What's verified |
|------|-----------------|
| IPC connection | Basic Unix socket connectivity |
| SCP broadcast/receive | SCP envelopes through IPC |
| Two-core communication | End-to-end via Rust overlays |
| SCP consensus (2-node) | Full consensus round |
| SCP consensus (10-node) | Ring topology consensus |
| TX submission | TX via IPC to mempool |
| TX flooding | TX propagation between peers |
| TX inclusion in ledger | Full TX lifecycle |
| Fee-per-op ordering | Fee priority in TX selection |
| Mempool eviction | Eviction at capacity |
| TX deduplication | Duplicate TX rejection |
| Mempool clear on externalize | Post-ledger cleanup |
| SCP latency under TX load | Stream independence proof |
| 15-node 2000 TPS stress | High-load consensus |
| Pre-Soroban TX set | Classic TX set handling |
| Soroban TX set | Soroban TX set handling |

---

## Known Issues and TODOs

### Bugs

1. **Fee overflow in mempool**: `fee * num_ops` in `FeePriority::cmp()`
   can overflow `u64` for very high fee values
   (`mempool.rs`, line ~64)

2. **Network TXs have fee=0**: TXs received from the network are added
   to the mempool with `fee=0, ops=1` because XDR parsing is not yet
   implemented (`main.rs`, line ~344). This breaks fee-based ordering
   for network-received TXs.

3. **INV fee_per_op always 0**: INV entries are created with
   `fee_per_op: 0` because the actual fee is not passed through
   (`libp2p_overlay.rs`, line ~961)

4. **source_account/sequence always zeroed**: `integrated.rs` does not
   parse XDR to extract account info, breaking per-account queries

5. **TxSetCache eviction is random**: Uses `HashMap::keys().next()`
   which has arbitrary iteration order, not FIFO or LRU (`txset.rs`)

### Unimplemented

1. **Soroban TX support**: TX set builder includes empty Soroban phase
   in XDR but no Soroban TXs are actually processed

2. **Network survey**: Not supported (legacy survey code removed)

3. **TX set fetch retry**: Retry logic is commented out in
   `libp2p_overlay.rs`. No timeout or retry to alternate peers.

4. **Periodic Kademlia re-bootstrap**: Only bootstraps once on
   `SetPeerConfig`; should re-bootstrap periodically

5. **Peer topology optimization**: Uses libp2p defaults. Not optimized
   for Stellar network characteristics (Tier1 full connectivity,
   watcher subscription, etc.)

6. **TX validation in overlay**: Mempool does no TX validation beyond
   dedup; DoS mitigation needed

7. **Config not fully wired**: `max_mempool_size` and
   `tx_push_peer_count` are defined in config but hardcoded in
   `integrated.rs` and `libp2p_overlay.rs` respectively

### Hash Functions

| Usage | Algorithm | Output |
|-------|-----------|--------|
| SCP/TX network dedup | Blake2b | 32 bytes |
| TX content hash (mempool) | SHA256 | 32 bytes |
| TX set hash | SHA256 | 32 bytes |
