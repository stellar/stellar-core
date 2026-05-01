# TX Propagation: INV / GETDATA / TX

Transactions are flooded with a **three-phase pull protocol** instead of
SCP's push-everything strategy. A node announces "I have TX X"
(INV_BATCH), peers that don't have it reply "send me X" (GETDATA), and
the originator answers with the full TX. This trades a bit of latency
for much lower bandwidth: in steady state a TX announcement costs ~40
bytes per peer instead of the full TX bytes-per-peer.

All three message types share **one** QUIC stream per peer
(`/stellar/tx/1.0.0`), distinguished by a one-byte type prefix. SCP and
TxSet have their own streams (see [transport.md](transport.md)), so
heavy TX traffic cannot stall consensus.

## Wire format

```
┌───────────┬──────┬───────────────────────────────────┬───────────────────────┐
│   Type    │ Code │              Payload              │       Direction       │
├───────────┼──────┼───────────────────────────────────┼───────────────────────┤
│ TX        │ 0x01 │ [raw tx bytes]                    │ Responder → requester │
│ INV_BATCH │ 0x02 │ [count:u32 BE][{hash:32, fee:i64 BE}...]   │ Announcer → all peers │
│ GETDATA   │ 0x03 │ [count:u32 BE][hash:32...]        │ Requester → announcer │
└───────────┴──────┴───────────────────────────────────┴───────────────────────┘
```

The `fee` carried in INV_BATCH entries is **fee-per-op**, intended to
let the receiver prioritize which TXs to pull first. (See [Known issues](#known-issues-and-todos)
— for TXs received from the network, this field is currently always 0.)

Encoding helpers live in `flood/inv_messages.rs`. Constants:

| Constant              | Value | Defined in                    |
|-----------------------|-------|-------------------------------|
| `INV_BATCH_MAX_SIZE`  | 1,000 | `flood/inv_messages.rs:65`    |
| `INV_BATCH_MAX_DELAY` | 100 ms| `flood/inv_batcher.rs:15`     |
| `GETDATA_PEER_TIMEOUT`| 1 s   | `flood/pending_requests.rs:13`|
| `GETDATA_TOTAL_TIMEOUT`| 30 s | `flood/pending_requests.rs:16`|

## Phase 1 — announce (INV)

When a node has a new TX (either submitted by Core via `SubmitTx`, or
received from a peer through GETDATA):

1. **Dedup**: check `tx_seen` (LRU, capacity 100,000,
   `libp2p_overlay.rs:308`). If already known, skip.
2. **Buffer**: store the full TX in `tx_buffer` (capacity 10,000, max
   age 60 s; `flood/tx_buffer.rs:11,14`) so we can serve later GETDATAs.
3. **Batch INVs**: for each connected peer, append an
   `InvEntry { hash, fee_per_op }` to that peer's batch in `InvBatcher`.

Batches are not flushed immediately. The batcher accumulates and flushes
when **either**:

- the batch reaches 1,000 entries, **or**
- 100 ms passes since the first entry was added.

A housekeeping task (`inv_getdata_housekeeping_task`,
`libp2p_overlay.rs:1749`) runs every **50 ms** and forces flushes for
batches that have hit the 100 ms timeout.

## Phase 2 — request (GETDATA)

When a node receives an `INV_BATCH` from a peer:

1. For each entry, check `tx_seen`. If we already have this TX, skip.
2. **Record sender as a source** in `inv_tracker` — an LRU map of
   `hash → Vec<PeerId>` (capacity 100,000, `flood/inv_tracker.rs:13`).
   The tracker is round-robin so we don't hammer a single peer.
3. If this is the **first** INV we've seen for this hash, add the hash
   to a GETDATA request to send back to the announcing peer.
4. Send the GETDATA batch to the announcer.
5. Record `PendingRequest { peer, sent_at, first_sent_at }` in
   `pending_getdata` for timeout tracking.

The key bandwidth optimization: **only the first announcer is asked**.
Subsequent peers who INV the same hash become backup sources for retry
without sending a duplicate GETDATA upfront.

## Phase 3 — respond (TX)

When a node receives a GETDATA:

1. For each requested hash, look up the full TX in `tx_buffer`.
2. If found, send `TX(data)` (`0x01` + raw bytes) back on the TX stream.
3. If not found (evicted or never had), increment
   `flood_unfulfilled_unknown` and skip.

## Receiving a TX response

When a node receives a `TX` message (a response to its own GETDATA):

1. **Dedup**: check `tx_seen`. If already have it, count as
   `flood_duplicate_recv` and skip.
2. **Measure latency**: remove from `pending_getdata`, compute
   `pull_latency = now - first_sent_at`.
3. **Buffer**: store in `tx_buffer` so we can serve future GETDATAs.
4. **Forward to Core**: `try_send` `TxReceived` on the **bounded** TX
   event channel (capacity 10,000, `libp2p_overlay.rs:46,398`). If the
   channel is full, the event is **dropped** — `tx_dropped_count` is
   incremented and a warning is logged every 1,000 drops. The TX is
   still buffered locally and will be served to other peers via
   GETDATA; only the upcall to Core is dropped.
5. **Relay**: announce to all peers that don't already know about this
   TX. Same INV-batching path as Phase 1, but with two filters: skip
   the peer we got it from, and skip any peer recorded in `inv_tracker`
   as having already announced this hash to us (they have it).

## Timeout and retry

Driven by the 50 ms housekeeping task
(`libp2p_overlay.rs:1749-1827`):

- **Per-peer timeout (1 s)**: if a GETDATA hasn't been answered in 1 s,
  retry to the next peer in `inv_tracker`'s source list (round-robin).
- **Total timeout (30 s)**: if the TX still hasn't arrived after 30 s
  across all retries, give up and increment `flood_abandoned_demands`.

## Backpressure

Only the **TX → Core** event channel is bounded. SCP and TxSet events
use an unbounded channel and are never dropped.

If Core can't keep up draining the TX channel:

- `try_send` fails → the `TxReceived` event is dropped (not queued).
- The TX remains in the overlay's `tx_buffer`, so other peers' GETDATAs
  are still answered.
- The TX is also still tracked in `tx_seen`, so duplicate INVs from
  other peers are still deduped.

This is by design: under TX-spam load, consensus continues to flow on
its own stream and the overlay degrades the *core upcall* gracefully
rather than back-pressuring the network layer.

## State summary

| Field                  | Type                                       | Capacity | Purpose                                |
|------------------------|--------------------------------------------|----------|----------------------------------------|
| `tx_seen`              | `RwLock<LruCache<[u8;32], ()>>`            | 100,000  | TX dedup                               |
| `tx_buffer`            | `RwLock<TxBuffer>`                         | 10,000 / 60 s | Full TX bodies for GETDATA responses |
| `inv_batcher`          | `RwLock<InvBatcher>`                       | per-peer | Outbound INV batching                  |
| `inv_tracker`          | `RwLock<InvTracker>`                       | 100,000  | Peer→TX advertisement tracking + RR    |
| `pending_getdata`      | `RwLock<PendingRequests>`                  | —        | GETDATA timeout/retry state            |

Plus the bounded TX event channel: 10,000 events, drops on overflow.

## Lifecycle: TX X appears on the network

```
Node A: Core submits TX X (SubmitTx IPC)
  → broadcast_tx(): hash, buffer, INV to {B, C, D} (batched)

Node B: receives INV_BATCH containing X from A
  → first INV for X → GETDATA {X} to A
  → pending_getdata[X] = { peer: A, sent_at: now }

Node C: receives INV_BATCH containing X from A
  → first INV for X → GETDATA {X} to A
  (Meanwhile B and C may relay-announce; if their INV reaches each
   other, the receiving side already has X in tx_seen and skips.)

Node A: receives GETDATA {X} from B
  → looks up tx_buffer[X] → sends TX(data) to B

Node B: receives TX response
  → dedup (new) → buffer → forward to Core
  → RELAY: INV {X} to {C, D}
  (skips A: A is in inv_tracker as the source)

Node C: receives TX from A + INV from B
  → TX arrives first → dedup (new) → buffer → forward to Core
  → RELAY: INV {X} to {D} (skips A and B)
  → INV from B arrives → already in tx_seen → skip

Node D: receives INV from A, INV from B (via C's relay)
  → first INV from A → GETDATA to A → receives TX
  → INV from C → already in tx_seen → skip
```

## Contrast with SCP flooding

| Aspect          | SCP                              | TX                                                    |
|-----------------|----------------------------------|-------------------------------------------------------|
| Model           | Push (full message immediately)  | Pull (INV → GETDATA → TX)                             |
| Batching        | None                             | INVs batched, 1,000 entries / 100 ms                  |
| Bandwidth       | O(msg_size × N) per hop          | O(40 B × N) per hop, full TX sent only on demand      |
| Backpressure    | Never dropped                    | Drops tolerated (re-requestable)                      |
| Relay decision  | Core decides                     | Overlay decides autonomously                          |
| Dedup scope     | `scp_seen` + `scp_sent_to`       | `tx_seen` + `inv_tracker`                             |
| Timeout/retry   | None (fire-and-forget)           | 1 s per-peer, 30 s total, round-robin                 |

## Known issues and TODOs

- **INV `fee_per_op` is always 0** for TXs received from the network
  (`libp2p_overlay.rs` ~line 961, and `main.rs:748` carries a TODO to
  parse XDR). Fee-based prioritization in INV is therefore disabled
  end-to-end for relayed TXs.
- **Mempool fee is 0 for network TXs** for the same reason (XDR parsing
  not wired); see [mempool.md](mempool.md).
- **No DoS scoring** at the overlay layer — a peer flooding INVs is not
  throttled.
