# Transport

The Rust overlay uses **QUIC over UDP** as its sole peer-to-peer transport, via
[`libp2p`](https://crates.io/crates/libp2p) with the `quic` feature.

## Listen address

- UDP, port = `peer_port + 1000` (so a node configured with `peer_port = 11625`
  listens for QUIC on `:12625`).
- libp2p multiaddr format: `/ip4/<addr>/udp/<port>/quic-v1`.
- Bind interface configured via `libp2p_listen_ip` (default `0.0.0.0`).

The `+1000` offset lets the legacy TCP overlay port stay reserved during
migration.

Defined in `main.rs` (multiaddr construction at `main.rs:285`,
`main.rs:122`).

## QUIC configuration

`libp2p_overlay.rs:369-371`:

| Parameter           | Value      | Notes                                       |
|---------------------|------------|---------------------------------------------|
| `keep_alive_interval` | 15s      | QUIC PING cadence to keep NAT bindings open |
| `max_idle_timeout`    | 60s      | Connection torn down if no traffic for 60s  |

`libp2p_overlay.rs:389`:

| Parameter             | Value | Notes                                |
|-----------------------|-------|--------------------------------------|
| `idle_connection_timeout` (swarm) | 300s | libp2p closes the swarm-level connection after 5 min idle (above the QUIC layer) |

## Encryption and identity

QUIC's TLS 1.3 handshake authenticates each peer with a libp2p
`identity::Keypair`. The keypair is generated at startup; the resulting
`PeerId` is the cryptographic identity used for all dedup, reconnection
tracking, and self-dial detection.

There is no separate handshake protocol on top of QUIC — peer authentication
is whatever libp2p's QUIC transport provides.

## Stream protocols

Each peer opens four outbound libp2p streams, multiplexed over a single
QUIC connection, using three protocol IDs:

| Protocol ID             | Purpose            | Role                                |
|-------------------------|--------------------|-------------------------------------|
| `/stellar/scp/1.0.0`    | SCP envelopes      | Push-based, length-prefixed frames |
| `/stellar/tx/1.0.0`     | TX flooding        | INV/GETDATA/TX (1-byte type prefix) |
| `/stellar/txset/1.0.0`  | TX set fetch       | Separate request and response streams, length-prefixed |

In addition, libp2p's `Identify` protocol runs as `/stellar/1.0.0`
(`libp2p_overlay.rs:380`). It exchanges peer-id + listen addresses on
connect; the overlay logs the result but does not currently feed it into
peer selection.

### Stream isolation

Each stream has its own QUIC stream-level flow control and its own per-peer
mutex on the send side. The command dispatcher also schedules network writes
as independent tasks, so:

- An outstanding TxSet write does not hold the dispatcher or the SCP stream
  mutex while it waits for the recipient.
- INV batches and TX response data flow on `/stellar/tx/1.0.0` only.
- Tx-set fetch requests have their own stream and mutex, so a large response
  to the same peer cannot hold up a request behind its frame or stream lock.
  Both streams use the existing tx-set protocol and encoding. The unchanged
  receiver already accepts multiple streams for that protocol, including
  requests from older peers on their combined request/response stream.
- Loss recovery on one stream does not require another stream to wait for
  that stream's missing bytes.

The request stream opens alongside the other streams when a peer connects
and reopens on demand after failure. The connection owns a fixed additional
stream per peer; this does not create a new stream for every fetch.

Streams still share connection-level flow control, congestion control, CPU,
and link bandwidth. This separation removes dispatcher and stream-lock
serialization; it does not guarantee SCP latency under link saturation.
These streams do not currently have different QUIC scheduling priorities.

## Frame formats

Both `/stellar/scp/1.0.0` and `/stellar/txset/1.0.0` use **length-prefixed
binary frames**:

```
[length:u32 big-endian][payload]
```

Maximum frame size: 16 MB (`libp2p_overlay.rs:42`,
`MAX_FRAME_SIZE`). `read_framed` / `write_framed` at
`libp2p_overlay.rs:1222` and `:1266`.

`/stellar/tx/1.0.0` uses a 1-byte type prefix instead (see
[tx-propagation.md](tx-propagation.md)).

> Note: the **IPC** protocol between Core and Overlay (Unix socket) uses
> *native-endian* lengths. Only the libp2p network frames use big-endian.

## Sending: `send_to_peer_stream`

`libp2p_overlay.rs:1109-1219`. Used for SCP, TX, and TxSet sends.

Behavior:

1. Looks up the peer's outbound stream from `peer_streams` (per-peer
   `PeerOutboundStreams { scp, tx, txset }`, each behind its own mutex).
2. If no stream is cached, opens one (`control.open_stream(peer_id, proto)`).
3. Writes the framed payload.
4. On error: drops the cached stream, reopens, and retries.

Retry policy: **3 attempts total** (`MAX_RETRIES = 2`, `libp2p_overlay.rs:1116`)
with backoff `10ms × (attempt + 1)` (so 10 ms, 20 ms, 30 ms). After the
final failure the send is abandoned and an error is returned to the caller;
the SCP/TX/TxSet broadcast paths log a warning and increment
`error_write` / similar metrics.

Per-stream mutexes serialize complete frames on a given peer/protocol stream.
Writes to different peers, or different protocols of the same peer, can
progress concurrently.

The dispatcher tracks tx-set responses, tx-set requests, SCP broadcasts,
direct SCP messages, and SCP-state requests in a `JoinSet`. It continues
polling the swarm and accepting commands while these writes wait for stream
locks, stream opening, socket capacity, or write-error retries. Stream opening
itself needs the swarm to be polled, so awaiting it inside the dispatcher can
otherwise prevent completion.

Tx-set responses acquire capacity **before** entering the command queue:
at most 64 responses and 256 MiB of framed tx-set payloads may be queued or
active. The caller waits when capacity is exhausted; the dispatcher remains
available to process SCP and connection events. Permits are held until the
write completes or fails. These limits cover admitted sends, not cached sets
or the input buffers owned by callers still waiting for admission. The peer
wire limit remains 16 MiB per message, including the StellarMessage
discriminant; oversized outgoing sets are logged and rejected.

These permits end at local write completion. Quinn's `flush()` returns
immediately; it does not wait for acknowledgement or remote receipt. The
limits therefore do not bound bytes still buffered inside QUIC or the network.

Fetch selection and pending-request reservation stay in the dispatcher, before
the write is spawned. This preserves duplicate-request suppression. A failed
write clears only its own reservation, so it cannot erase a request reassigned
after a disconnect. This change does not add a fetch timeout or retry policy.

On shutdown, the command receiver and swarm are closed before outstanding
dispatcher sends are cancelled. Waiting producers wake up and send permits
are released.

`cargo test -p stellar-overlay --lib dispatch_tests` covers blocked writes,
actual SCP/tx-set delivery across three QUIC nodes, stream reopening and
framing, admission limits, shutdown, and fetch-reservation races. The transport
fixture contains 6,000 envelopes and exceeds 4 MiB; it is not a SAC throughput
benchmark.
The fetch-isolation regression holds a response stream lock while requests
reach that same peer over QUIC, checks stream reuse and reopening, then
verifies that the original response arrives intact after release. Its initial
request timed out before the streams were separated.

## Inbound stream handling

For each accepted stream, the overlay spawns a long-lived task that loops
on `read_framed` (or, for `/stellar/tx/1.0.0`, reads the 1-byte type and
dispatches). On read error the task exits and the stream is dropped from
state; if the peer is still connected, libp2p will accept a fresh stream
on demand.

## Self-dial prevention

The overlay collects local interface addresses at startup
(`collect_local_addrs`, `main.rs:367`), and later resolved peer addresses
are checked against this set before dialing. Important in Kubernetes-style
deployments where a pod's hostname can resolve back to itself.

## What's not here

- **No TCP fallback.** QUIC only. A peer that cannot reach the QUIC port
  (UDP-blocked network) will not connect.
- **No bandwidth shaping or QoS** — relies on QUIC stream-level fairness.
- **No custom encryption layer** on top of QUIC TLS 1.3.
