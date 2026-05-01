# Rust Overlay Design

**Status**: Prototype.

The Rust overlay is a **separate process** that handles all peer-to-peer
networking for `stellar-core`. It communicates with the C++ core via a
Unix domain socket.

This document is the high-level overview. For each subsystem there is a
dedicated doc under [`docs/rust-overlay/`](rust-overlay/):

- [Transport](rust-overlay/transport.md) — QUIC, libp2p, stream framing.
- [Peer connections](rust-overlay/peer-connections.md) — connection
  strategy, DNS, reconnect.
- [SCP flooding](rust-overlay/scp-flooding.md) — push-based SCP
  propagation.
- [TX propagation](rust-overlay/tx-propagation.md) — pull-based
  INV/GETDATA TX flooding.
- [TX-set fetching](rust-overlay/txset-fetching.md) — fetching and
  caching nominated TX sets.
- [Mempool](rust-overlay/mempool.md) — fee-ordered pending-TX store.
- [Core ↔ Overlay IPC](rust-overlay/ipc.md) — Unix socket protocol.

## Why a separate process

- **Process isolation**: a panic, parse bug, or memory corruption in the
  network-facing code cannot crash the consensus state machine. Restart
  / upgrade the overlay independently of Core.
- **Smaller blast radius for CVEs**: untrusted-input parsing (peer
  messages, DNS, TLS handshake) lives outside the Core process.
- **Memory safety**: Rust eliminates the use-after-free / data-race
  classes that have historically bitten the C++ overlay.

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
                                       │ length-prefixed binary frames
┌──────────────────────────────────────│───────────────────────────┐
│                     stellar-overlay (Rust)                       │
│                                      │                           │
│  ┌───────────────────────────────────▼──────────────────────────┐│
│  │                       Main event loop                        ││
│  │  • Reads IPC messages from Core                              ││
│  │  • Consumes libp2p events (SCP / TxSet / peer / TX)          ││
│  │  • Drives reconnect timer                                    ││
│  └──────────┬──────────────────────────────────┬────────────────┘│
│             │                                  │                 │
│  ┌──────────▼──────────┐          ┌────────────▼───────────────┐ │
│  │  Mempool + flood    │          │  libp2p Swarm (QUIC)       │ │
│  │  (integrated.rs,    │          │                            │ │
│  │   flood/)           │          │  Behaviours:               │ │
│  │                     │          │  • libp2p-stream (3 protos)│ │
│  │  • Fee-ordered pool │          │  • Identify (informational)│ │
│  │  • INV batching     │          │                            │ │
│  │  • GETDATA tracking │          │  Transport: QUIC over UDP  │ │
│  │  • TX set cache     │          │                            │ │
│  └─────────────────────┘          └────────────────────────────┘ │
│                                           │                      │
│                           ┌───────────────┼───────────────┐      │
│                           ▼               ▼               ▼      │
│                      [Peer 1]        [Peer 2]        [Peer N]    │
│                      SCP stream      SCP stream      SCP stream  │
│                      TX stream       TX stream       TX stream   │
│                      TxSet stream    TxSet stream    TxSet str.  │
└──────────────────────────────────────────────────────────────────┘
```

## Properties at a glance

- **Transport**: QUIC over UDP, via libp2p. TLS 1.3, 0-RTT reconnects,
  per-stream flow control. Listen port = `peer_port + 1000`. See
  [transport.md](rust-overlay/transport.md).
- **Stream independence**: SCP, TX, and TxSet each get their own libp2p
  stream (`/stellar/scp/1.0.0`, `/stellar/tx/1.0.0`,
  `/stellar/txset/1.0.0`). A multi-MB TxSet write cannot stall a
  500-byte SCP envelope. This is the single biggest design win over the
  legacy single-TCP-stream overlay.
- **Peer membership is Core-driven**. There is no peer-discovery
  protocol — no Kademlia, no peer exchange, no gossip. The overlay
  connects to addresses Core sends via `SetPeerConfig` and accepts any
  inbound dial. Reconnects to configured peers are handled with
  exponential backoff; targeted reconnect on disconnect plus a 30-second
  safety-net sweep. See
  [peer-connections.md](rust-overlay/peer-connections.md).
- **SCP is pushed; TX is pulled**. SCP envelopes flood immediately on
  receipt. TXs use a three-phase INV/GETDATA protocol with batched
  announcements and per-peer/total timeouts. See
  [scp-flooding.md](rust-overlay/scp-flooding.md) and
  [tx-propagation.md](rust-overlay/tx-propagation.md).
- **Mempool lives in the overlay**. Fee-ordered, capacity 100,000,
  300-second max age. Core queries it for nomination via `GetTopTxs`.
  See [mempool.md](rust-overlay/mempool.md).
- **Backpressure asymmetry**. SCP and TxSet events to Core are on an
  unbounded channel and never drop. TX events are on a bounded channel
  (10,000) and may drop under load — TXs are re-fetchable via the same
  INV/GETDATA protocol, so this is acceptable. See
  [ipc.md](rust-overlay/ipc.md#channel-discipline).

## What changed vs. the legacy C++ overlay

| Property                    | Legacy (C++)                              | Rust overlay                                          |
|-----------------------------|-------------------------------------------|-------------------------------------------------------|
| Process boundary            | In-process with consensus                 | Separate process, IPC over Unix socket                |
| Transport                   | TCP + custom auth                         | QUIC (TLS 1.3 + multiplexing) via libp2p              |
| Stream isolation            | Single TCP connection per peer            | Three logical streams per peer over one QUIC conn    |
| Memory-safety class         | C++                                       | Safe Rust                                             |
| TX flooding                 | Pull-based (existing INV/GETDATA scheme)  | Pull-based (INV/GETDATA), reimplemented              |
| SCP flooding                | Push-based                                | Push-based                                            |
| Peer discovery              | Built-in PEERS gossip                     | None — Core-driven via `SetPeerConfig`               |
| Mempool location            | In Core                                   | In overlay process                                    |

## Configuration

TOML, parsed at startup (`config.rs`):

| Field               | Type          | Default                       | Description                                         |
|---------------------|---------------|-------------------------------|-----------------------------------------------------|
| `core_socket`       | `PathBuf`     | `/tmp/stellar-overlay.sock`   | IPC socket path                                     |
| `listen_addr`       | `SocketAddr`  | `0.0.0.0:11625`               | Legacy peer-port placeholder                        |
| `libp2p_listen_ip`  | `String`      | `0.0.0.0`                     | QUIC bind interface                                 |
| `peer_port`         | `u16`         | `11625`                       | Base port. QUIC listens on `peer_port + 1000`       |
| `target_outbound_peers` | `usize`   | `8`                           | **Defined but unused**                              |
| `max_inbound_peers` | `usize`       | `64`                          | **Defined but unused**                              |
| `preferred_peers`   | `Vec<SocketAddr>` | `[]`                       | Static preferred peers (see note below)             |
| `known_peers`       | `Vec<SocketAddr>` | `[]`                       | Static known peers (see note below)                 |
| `tx_push_peer_count`| `usize`       | `8`                           | **Defined but unused** in network code              |
| `max_mempool_size`  | `usize`       | `100_000`                     | **Defined but unused** — mempool is hardcoded       |
| `http_addr`         | `Option<SocketAddr>` | `127.0.0.1:11626`      | HTTP server (TX submission, status)                 |
| `log_level`         | `String`      | `info`                        | `tracing` log level                                 |

> **About `known_peers` / `preferred_peers` in the file**: peer
> membership at runtime is set by Core via the `SetPeerConfig` IPC
> message, *not* by the values in this file. The static fields here are
> kept for tooling and tests but are not the source of truth.

## Code structure

```
overlay/
├── Cargo.toml
├── src/
│   ├── main.rs              # Entry point, event loop, IPC handlers,
│   │                        # SetPeerConfig, reconnect, DNS retry
│   ├── lib.rs               # Public module exports
│   ├── config.rs            # TOML configuration parsing
│   ├── libp2p_overlay.rs    # libp2p swarm, stream handling,
│   │                        # SCP/TX/TxSet send paths, housekeeping
│   ├── integrated.rs        # Mempool manager + high-level overlay API
│   ├── metrics.rs           # Atomic-counter metrics for /metrics
│   ├── ipc/
│   │   ├── mod.rs
│   │   ├── messages.rs      # IPC message types and codec
│   │   └── transport.rs     # Unix-socket read/write + dispatcher
│   ├── flood/
│   │   ├── mod.rs
│   │   ├── mempool.rs       # Fee-ordered TX mempool
│   │   ├── txset.rs         # TX set cache + XDR builder
│   │   ├── inv_messages.rs  # INV/GETDATA wire format
│   │   ├── inv_batcher.rs   # Per-peer INV batching
│   │   ├── inv_tracker.rs   # Peer→TX advertisement tracking
│   │   ├── pending_requests.rs  # GETDATA timeout/retry
│   │   └── tx_buffer.rs     # TX storage for GETDATA responses
│   └── http/
│       └── mod.rs           # HTTP server (TX submission, status)
└── tests/
    └── e2e_binary.rs        # Binary integration tests
```

## Known issues and TODOs

These limitations live across multiple subsystems; details are in the
relevant subsystem doc.

- **Network-received TXs land in the mempool with `fee=0, num_ops=1,
  account=0, sequence=0`** — XDR parsing of `TransactionEnvelope` is not
  yet implemented (`main.rs:748`, `integrated.rs:144`). This breaks
  fee ordering, account grouping, and the `fee_per_op` carried in
  outbound INV entries for relayed TXs.
- **TX-set fetch retry is commented out** (`libp2p_overlay.rs:1829-1916`).
  A pending fetch to a silent peer leaks until the peer disconnects;
  Core's own retry policy is the only safety net.
- **`max_mempool_size`, `tx_push_peer_count`, `target_outbound_peers`,
  and `max_inbound_peers` are unused**. The mempool is hardcoded to
  100,000 entries; the overlay does not enforce inbound or outbound
  connection limits.
- **TxSet cache eviction under capacity pressure is non-deterministic**
  (`HashMap::keys().next()`, `txset.rs:48`). Per-ledger
  `evict_before(seq-12)` keeps the cache bounded in practice.
- **`evict_expired` on the mempool is implemented but not scheduled** —
  old TXs sit in the mempool until pushed out by capacity-based
  eviction.
- **No DoS scoring or peer banning** at the overlay layer. A peer
  flooding INVs is not throttled.
- **No survey support**. Legacy network-survey messages are not
  implemented.
- **No fee overflow guard** in mempool ordering: `fee * num_ops` is
  computed in `u64` and can overflow for pathological values
  (`mempool.rs:64`).

## Hash functions

| Usage                       | Algorithm | Output bytes |
|-----------------------------|-----------|--------------|
| SCP / TX network dedup      | Blake2b   | 32           |
| TX content hash (mempool)   | SHA-256   | 32           |
| TX set hash                 | SHA-256   | 32           |
