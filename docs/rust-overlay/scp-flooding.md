# SCP Flooding

SCP envelopes are flooded **push-first** on a dedicated QUIC stream
(`/stellar/scp/1.0.0`). They are small, latency-sensitive, and never
dropped — there is no batching, no backpressure, no INV/GETDATA round
trip. The overlay layer is a dumb pipe: it dedups and forwards, but it
does not autonomously decide to relay. Core is the relay decision-maker.

## The two flows

### Outbound: `broadcast_scp(envelope)`

`libp2p_overlay.rs:672-749`. Called from two places:

1. **Core → Overlay** (`main.rs:961-978`): Core creates an SCP message
   locally and tells the overlay to broadcast it via the `BroadcastScp`
   IPC message.
2. **Peer → Overlay → Core → Overlay** (the relay loop): a peer sends
   us an envelope, the overlay forwards it to Core via the `ScpReceived`
   event, Core processes it and calls `BroadcastScp` to re-broadcast.

Logic:

1. **Hash** the envelope with Blake2b (32 bytes).
2. **Mark seen** in `scp_seen` LRU (capacity 10,000,
   `libp2p_overlay.rs:305`). This dedups against later inbound copies.
3. **Look up `scp_sent_to`** for this hash → set of peers that already
   have it. Compute `targets = all_connected_peers \ already_sent`.
4. If `targets` is empty, skip — every peer already has it.
5. **Record intent**: insert all target peers into `scp_sent_to` *before*
   sending. This ensures concurrent calls to `broadcast_scp` for the
   same hash do not double-send.
6. **Spawn parallel sends**: one `tokio::spawn` per target peer, calling
   `send_to_peer_stream(StreamType::Scp, ...)`. Sends do not block the
   event loop or each other.

### Inbound: SCP stream handler

`libp2p_overlay.rs:1291-1368`. Each peer has a dedicated inbound SCP
stream task that loops on length-prefixed reads:

1. **Read frame**: 4-byte big-endian length prefix + payload (see
   [transport.md](transport.md#frame-formats)).
2. **State-request shortcut**: if the payload is exactly **4 bytes**, it
   is interpreted as a `GET_SCP_STATE` request — the four bytes are a
   ledger sequence number. The handler emits an `ScpStateRequested`
   event and continues.
3. **Hash + dedup**: Blake2b the envelope, check `scp_seen`. If already
   seen, log as duplicate and skip.
4. **Record sender** in `scp_sent_to`: insert this peer's `PeerId` for
   the hash. This guarantees that when Core later asks us to re-broadcast,
   we don't echo back to the peer who just sent it.
5. **Forward to Core**: emit `ScpReceived { envelope, from }` on the
   *unbounded* event channel. Core processes it and calls
   `BroadcastScp` to relay.

## Relay walkthrough

3-node chain `A → B → C`:

```
A: Core builds envelope
A: broadcast_scp() → hash, mark seen, send to B and C
B: inbound SCP handler → hash, mark seen, record A in sent_to
B: emit ScpReceived → main.rs forwards to Core
B: Core processes, calls BroadcastScp
B: broadcast_scp() → hash, check sent_to → A already has it
                  → send only to C
C: inbound SCP handler → hash, mark seen, record B in sent_to
C: emit ScpReceived → main.rs forwards to Core
C: Core processes, calls BroadcastScp
C: broadcast_scp() → hash, check sent_to → A and B already have it
                  → skip (nobody left)
```

Two distinct dedup structures keep this from looping:

- **`scp_seen`** prevents the same envelope from being processed twice
  per node, even if multiple peers send it to us.
- **`scp_sent_to`** prevents the same envelope from being sent to the
  same peer twice. Crucially, the inbound handler inserts the *sender*
  into this map before Core re-broadcasts, so the relay never bounces
  the message back to its origin.

## State

`libp2p_overlay.rs:305-307`:

| Field          | Type                                        | Capacity | Purpose                                       |
|----------------|---------------------------------------------|----------|-----------------------------------------------|
| `scp_seen`     | `RwLock<LruCache<[u8; 32], ()>>`            | 10,000   | Inbound dedup (Blake2b hash of envelope)      |
| `scp_sent_to`  | `RwLock<LruCache<[u8; 32], HashSet<PeerId>>>` | 10,000 | Per-hash set of peers we already sent to     |

LRU eviction means an envelope older than 10,000 unique SCP messages is
forgotten — irrelevant for consensus rounds (which fit in well under
10,000 envelopes), but a worth-knowing detail if catch-up replays a
massive backlog.

## Channel discipline

The event channel from overlay to Core for SCP / TxSet is **unbounded**
(`libp2p_overlay.rs:396`). SCP messages are *never* dropped at the
overlay-Core boundary. (TX events are bounded — see
[tx-propagation.md](tx-propagation.md).)

## Send mechanics

Each peer has its own SCP stream behind its own mutex. Sends are
fire-and-forget per peer: `tokio::spawn` per recipient. A slow or failed
peer does not block sends to others.

The send retries up to 3 times with 10 ms / 20 ms / 30 ms backoff, and
will reopen the stream if needed (see
[transport.md](transport.md#sending-send_to_peer_stream)). After 3 failed
attempts the send is abandoned and the broadcast path logs a warning.

## Properties worth stating

- **No relay at the overlay layer.** The flow always goes peer → overlay
  → Core → overlay → peers. Core decides whether to re-broadcast; the
  overlay never autonomously relays.
- **No priority or ordering.** All SCP messages are treated equally and
  sent in the order they arrive.
- **No pull model.** SCP is always pushed. Compare TX, which uses
  INV/GETDATA.
- **No fan-out limit.** Every connected peer gets every SCP message. In
  a 20-node mesh, a single envelope is sent ~19 times by the originator
  and re-sent ~18 times by each first-hop relay (though `scp_sent_to`
  prevents actual duplicates on the wire).
- **No batching.** Each SCP envelope is its own frame, sent immediately.
- **Stream isolation.** SCP shares a QUIC connection with TX/TxSet but
  not a stream. A 10-MB TxSet write cannot stall a 500-byte SCP
  message.

## Rough bandwidth shape

For an `N`-node mesh broadcasting a single envelope of size `s`:

- The originator sends `s × (N-1)` bytes on its SCP streams.
- Each first-hop receiver re-sends to `N-2` peers (everyone except the
  origin), but `scp_sent_to` causes those to dedup against the
  origin's direct sends.
- Net wire bytes: `O(s × N)` per envelope per hop, with `scp_seen`
  preventing receive-side double-processing.

## What's not here

- **No SCP state pull on demand.** The 4-byte state-request frame on
  the SCP stream is responded to from Core's local state; there is no
  retry, no fan-out, no aggregated response.
- **No quorum-set pull.** Quorum sets are delivered separately via the
  IPC `QuorumSetAvailable` message.
