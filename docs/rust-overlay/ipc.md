# Core ↔ Overlay IPC

The C++ `stellar-core` process and the Rust `stellar-overlay` process
talk to each other over a **single Unix domain socket**. The protocol
is intentionally simple: length-prefixed binary messages, one type per
direction, no acks.

Source:
- Rust: `overlay/src/ipc/messages.rs`, `overlay/src/ipc/transport.rs`
- C++: `src/overlay/IPC.h` (the comment in `messages.rs` notes that the
  Rust types mirror the C++ definitions exactly).

## Transport

- Unix domain socket. Path defaults to `/tmp/stellar-overlay.sock`,
  configurable via `core_socket` in `config.toml`.
- Single bidirectional stream. Both sides read and write
  asynchronously.
- The Rust side wraps the socket in blocking I/O dispatched to
  `tokio::task::spawn_blocking` — a comment in `messages.rs:160`
  explains this is because async UDS with Tokio is "tricky on some
  platforms".

## Frame format

```
[type:u32 native-endian][length:u32 native-endian][payload : length bytes]
```

`messages.rs:4` and `messages.rs:140-210`:

| Field    | Bytes | Notes                                                |
|----------|-------|------------------------------------------------------|
| `type`   | 4     | `MessageType` discriminant. Native-endian (matches C++ struct layout). |
| `length` | 4     | Payload length in bytes. Native-endian.              |
| `payload`| `length` | Type-specific bytes. May be empty.                |

Maximum payload: **16 MB** (`messages.rs:154`). Frames larger than that
are rejected with `InvalidData`.

> Native-endian is unusual for a wire protocol but safe here because
> Core and Overlay always run on the same host. The libp2p network
> protocols (SCP / TX / TxSet streams) use big-endian — see
> [transport.md](transport.md#frame-formats).

## Message types

`messages.rs:14-80`. Discriminant ranges:

- `1..=99` — Core → Overlay
- `100..=199` — Overlay → Core

### Core → Overlay

| ID | Name                  | Payload                                      | Purpose                                          |
|---:|-----------------------|----------------------------------------------|--------------------------------------------------|
|  1 | `BroadcastScp`        | `[scp_envelope]`                             | Flood SCP envelope to peers                       |
|  2 | `GetTopTxs`           | `[count:u32]`                                | Get top-N TXs by fee for nomination               |
|  3 | `RequestScpState`     | `[ledger_seq:u32]`                           | Ask peers for SCP state at a ledger seq           |
|  4 | `LedgerClosed`        | `[ledger_seq:u32][ledger_hash:32]`           | Notify ledger advancement; triggers cache eviction |
|  5 | `TxSetExternalized`   | `[txset_hash:32][num:u32][tx_hash:32]…`      | TX set was applied; remove TXs from mempool       |
|  6 | `ScpStateResponse`    | `[count:u32][env_len:u32][env]…`             | SCP state for a peer that asked us                |
|  7 | `Shutdown`            | (empty)                                      | Graceful shutdown                                 |
|  8 | `SetPeerConfig`       | UTF-8 JSON (see below)                       | Configure peer addresses                          |
| 10 | `SubmitTx`            | `[fee:i64 LE][num_ops:u32 LE][tx_xdr]`       | Submit TX for flood + mempool                     |
| 11 | `RequestTxSet`        | `[hash:32][slot:u32 LE]`                     | Fetch TX set body by hash                         |
| 12 | `CacheTxSet`          | `[hash:32][slot:u32 LE][txset_xdr]`          | Tell overlay to cache a locally-built TX set      |
| 13 | `RequestOverlayMetrics` | (empty)                                    | Request metrics snapshot                          |

### Overlay → Core

| ID  | Name                   | Payload                                      | Purpose                                       |
|----:|------------------------|----------------------------------------------|-----------------------------------------------|
| 100 | `ScpReceived`          | `[scp_envelope]`                             | An SCP envelope arrived from a peer           |
| 101 | `TopTxsResponse`       | `[count:u32][len:u32][tx]…`                  | Reply to `GetTopTxs`                          |
| 102 | `PeerRequestsScpState` | `[ledger_seq:u32]`                           | A peer wants our SCP state                    |
| 103 | `TxSetAvailable`       | `[hash:32][txset_xdr]`                       | Reply to `RequestTxSet` (cache or peer fetch) |
| 104 | `QuorumSetAvailable`   | (quorum set XDR)                             | A quorum set arrived                          |
| 105 | `OverlayMetricsResponse` | UTF-8 JSON                                | Reply to `RequestOverlayMetrics`              |

### Notes on individual fields

- **Endianness inside the payload**: the *frame* header is native-endian,
  but several payloads use explicit little- or big-endian fields:
  - `SubmitTx`: `fee` and `num_ops` are little-endian
    (`main.rs:1102-1103`).
  - `LedgerClosed`, `RequestScpState`, `TxSetExternalized` ledger seq
    fields: little-endian (`main.rs:1121, 1143, 1172`).
  - `RequestScpState` peer-side wire frame on `/stellar/scp/1.0.0`:
    big-endian (network).
  This is asymmetric and not all paths are documented — when adding new
  message types, prefer little-endian for IPC payload integers and
  big-endian for libp2p stream payloads, matching the existing patterns.
- **`SetPeerConfig`** payload is JSON, not binary:
  ```json
  { "known_peers": ["host:port", …],
    "preferred_peers": ["host:port", …],
    "listen_port": 11625 }
  ```
  See [peer-connections.md](peer-connections.md).

## Channel discipline

The App loop consumes Core IPC and a separate libp2p event channel. Network
transactions enter the mempool command FIFO directly, without passing through
App or sending a per-transaction IPC upcall to Core.

| Channel or admission point | Bound | Carries |
|---|---|---|
| libp2p App events | Unbounded; no overflow dropping | SCP, TxSet, and peer events |
| Network mempool admission | 10,000 queued plus active inserts; nonblocking refusal | Network TXs |
| Mempool command FIFO | Unbounded channel; network producers require admission permits | Admissions, Core submissions, removals, queries |
| Core IPC sender | Unbounded | Outbound IPC messages |
| Core IPC receiver | Existing mpsc channel | Inbound IPC messages |

Network saturation does not prevent removal or query commands from entering
the FIFO. Refused admissions are not marked seen, allowing a later response
to succeed. Unbounded queues can accumulate work if consumers stall, and sends
to closed channels still fail. See [ordered admission](mempool.md#tx-received-over-the-network)
and [TX backpressure](tx-propagation.md#backpressure).

## Failure modes

- **Socket closed** (Core process exits): the receiver returns `None`,
  the overlay's main loop exits. The overlay process terminates.
- **Frame parse error** (e.g. bad magic, oversized length): connection
  is treated as broken; depending on which side detected it, either
  process logs the error and shuts down. There is no resync.
- **Unknown message type**: returns `InvalidData`. Adding a new message
  type requires updating `MessageType::try_from` (in `messages.rs`).

There is no version negotiation. Core and Overlay must be built from
the same source revision; running mismatched binaries will manifest as
parse errors on the first unknown message type.
