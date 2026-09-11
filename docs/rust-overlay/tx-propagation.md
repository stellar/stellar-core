# TX Propagation: INV / GETDATA / TX

Transactions are flooded with a **three-phase pull protocol** instead of
SCP's push-everything strategy. A node announces "I have TX X"
(INV_BATCH), peers that don't have it reply "send me X" (GETDATA), and
the originator answers with the full TX. This trades a bit of latency
for much lower bandwidth: in steady state a TX announcement costs ~40
bytes per peer instead of the full TX bytes-per-peer.

All three message types share **one** QUIC stream per peer
(`/stellar/tx/1.0.0`), distinguished by the `StellarMessage` XDR union arm.
Control and tx-set data use separate, higher-priority streams (see
[transport.md](transport.md)). Stream locks are independent, while connection
flow control, congestion control, and bandwidth remain shared.

## Wire format

All three message types are length-prefixed `StellarMessage` XDR frames
(see [transport.md](transport.md#frame-formats)):

| Message   | `StellarMessage` arm | Payload                        | Direction             |
|-----------|----------------------|--------------------------------|-----------------------|
| TX        | `Transaction`        | Full `TransactionEnvelope`     | Responder → requester |
| INV_BATCH | `FloodAdvert`        | TX hashes (32 bytes each)      | Announcer → all peers |
| GETDATA   | `FloodDemand`        | Requested TX hashes            | Requester → announcer |

`FloodAdvert` carries **hashes only** — the wire format has no fee
field. Each announced entry has an internal `fee_per_op` (read off the
shared `ValidatedTx`), but it is dropped at encode time, and the
receive side records announced entries with fee 0
(`flood/inv_messages.rs`). Receiver-side prioritization of which TXs to
pull first by announced fee is therefore not possible with this wire
format. (See [Known issues](#known-issues-and-todos).)

The codec lives in `flood/inv_messages.rs`: the single strict decode of
an inbound `Transaction` message also mints the `Arc<ValidatedTx>` (fee,
op count, sha256 hash, canonical bytes) that flows through the rest of
the pipeline. Constants:

| Constant              | Value | Defined in                    |
|-----------------------|-------|-------------------------------|
| `INV_BATCH_MAX_SIZE`  | 1,000 | `flood/inv_messages.rs:24`    |
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
2. If found, send a `StellarMessage::Transaction` frame back on the TX
   stream — framed by prefixing the union discriminant to the canonical
   envelope bytes we already hold, with no decode/re-encode.
3. If not found (evicted or never had), increment
   `flood_unfulfilled_unknown` and skip.

## Receiving a TX response

When a node receives a `TX` message (a response to its own GETDATA):

1. **Dedup and admit**: under the existing `tx_seen` lock, skip known hashes
   or call `try_submit_network_tx` without awaiting. This enqueues admission
   directly into the same mempool command FIFO used by Core removals.
2. **Mark seen only on success**. Admission is bounded at 10,000 queued plus
   active network insertions. If full or closed, drop this attempt, update the
   drop metrics, and return without marking the hash seen or clearing the
   pending GETDATA. A later response can be retried.
3. **Measure latency**: remove from `pending_getdata` and record pull latency.
4. **Buffer**: store in `tx_buffer` to serve future GETDATAs.
5. **Relay**: use INV batching to announce to peers that have not advertised
   the hash, excluding the peer that supplied the response.

There is no separate App TX event queue or per-transaction upcall to Core.
See [ordered admission](mempool.md#tx-received-over-the-network) for removal ordering and capacity
lifetime.

## Timeout and retry

Driven by the 50 ms housekeeping task
(`libp2p_overlay.rs:1749-1827`):

- **Per-peer timeout (1 s)**: if a GETDATA hasn't been answered in 1 s,
  retry to the next peer in `inv_tracker`'s source list (round-robin).
- **Total timeout (30 s)**: if the TX still hasn't arrived after 30 s
  across all retries, give up and increment `flood_abandoned_demands`.

## Backpressure

Network admission uses a shared semaphore to bound queued and active inserts.
The network reader never waits for capacity. Removal and query commands need
no admission permit; they retain FIFO ordering behind already-admitted TXs.
Local Core submissions retain their existing unbounded enqueue policy.

Refused admissions do not enter `tx_seen` or the relay buffer on that attempt.
Pending GETDATA retry remains available. SCP, TxSet, and peer events use the
separate unbounded App event channel and do not share the TX admission limit.

## State summary

| Field                  | Type                                       | Capacity | Purpose                                |
|------------------------|--------------------------------------------|----------|----------------------------------------|
| `tx_seen`              | `RwLock<LruCache<[u8;32], ()>>`            | 100,000  | TX dedup                               |
| `tx_buffer`            | `RwLock<TxBuffer>`                         | 10,000 / 60 s | Full TX bodies for GETDATA responses |
| `inv_batcher`          | `RwLock<InvBatcher>`                       | per-peer | Outbound INV batching                  |
| `inv_tracker`          | `RwLock<InvTracker>`                       | 100,000  | Peer→TX advertisement tracking + RR    |
| `pending_getdata`      | `RwLock<PendingRequests>`                  | —        | GETDATA timeout/retry state            |

Network admissions share 10,000 permits held through mempool insertion.

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
  → dedup (new) → enqueue mempool admission → mark seen → buffer
  → RELAY: INV {X} to {C, D}
  (skips A: A is in inv_tracker as the source)

Node C: receives TX from A + INV from B
  → TX arrives first → dedup (new) → enqueue mempool admission → mark seen → buffer
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

- **No DoS scoring** at the overlay layer — a peer flooding INVs is not
  throttled.
