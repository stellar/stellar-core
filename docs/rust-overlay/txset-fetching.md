# TX Set Fetching

A TX set is the full list of transaction envelopes a validator nominates
for a ledger. Validators reference TX sets by hash inside SCP messages,
so any node that hasn't seen the body needs to fetch it from a peer
before it can validate the SCP round.

The Rust overlay handles this on a dedicated stream
(`/stellar/txset/1.0.0`), separate from the SCP and TX streams. This
keeps multi-megabyte TxSet transfers from stalling consensus or TX
flooding.

## Stream wire format

`/stellar/txset/1.0.0` uses **length-prefixed frames** (4-byte BE length,
see [transport.md](transport.md#frame-formats)). The payload is
distinguished by length:

| Frame size | Meaning  | Payload                                |
|-----------:|----------|----------------------------------------|
| 32 bytes   | Request  | The 32-byte TX set hash                |
| > 32 bytes | Response | `[hash:32][txset XDR ...]`             |

Inbound TxSet stream handler: `libp2p_overlay.rs:1670-1740`. The 32-byte
heuristic at line 1680 is how the receiver tells request from response.

## Two paths

### Core asks Overlay for a TX set

Trigger: `RequestTxSet` IPC message (`main.rs:1023-1065`). Payload is a
32-byte hash.

Resolution order:

1. **Local cache lookup** (`tx_set_cache`). On hit, the overlay
   immediately replies with a `TxSetAvailable` IPC message containing
   the cached XDR. No network traffic.
2. **Cache miss** → mark the hash as pending, then call
   `libp2p_handle.fetch_txset(hash)`. The eventual `TxSetReceived` event
   from libp2p triggers a `TxSetAvailable` IPC reply to Core.

### Peer asks Overlay for a TX set

A peer sends a 32-byte frame on its TxSet stream. The inbound handler
emits a `TxSetRequested { hash, from }` event
(`libp2p_overlay.rs:1691`). The main loop looks up the cache and, on
hit, calls `send_txset_response(peer, hash, xdr)` which sends back
`[hash][xdr]` on the TxSet stream (`libp2p_overlay.rs:913-940`).

On miss there is no reply — the requester is responsible for retrying
to a different peer (which currently is not implemented; see
[Known gaps](#known-gaps)).

## fetch_txset peer selection

`libp2p_overlay.rs:810-911`.

1. **Dedup**: if a request for this hash is already pending to a still-
   connected peer, do nothing (`pending_txset_requests`).
2. **Prefer the SCP source**: `txset_sources` is an LRU populated when
   we receive an SCP message that references a TX set hash, mapping
   `hash → PeerId`. If that peer is still connected, fetch from them.
3. **Fallback**: pick any connected peer (`peer_streams.keys().next()`).
4. **No peers connected**: log and return — the request is *not*
   queued. Core will retry on its own schedule.

The pending-request entry records `(peer, Instant)` so we can log
fetch latency when the response arrives.

## TX set cache

`flood/txset.rs`.

```rust
pub struct TxSetCache {
    by_hash: HashMap<Hash256, CachedTxSet>,
    max_size: usize,
}
```

Each `CachedTxSet` carries the hash, the XDR bytes, the ledger sequence
it was built for, and the contained TX hashes (used by Core to remove
externalized TXs from the mempool — see [mempool.md](mempool.md)).

### Eviction

- **Capacity-based**: if `by_hash.len() >= max_size`, the cache evicts
  one arbitrary entry chosen via `keys().next()` (`txset.rs:48`). This
  is **not LRU and not FIFO** — it depends on `HashMap` iteration order,
  which is randomized per-process. For TX sets specifically this matters
  much less than for mempool/INV state, because cache lifetime is mainly
  controlled by `evict_before`.
- **Ledger-based**: on `LedgerClosed { seq }`, the cache calls
  `evict_before(seq.saturating_sub(12))` — TX sets older than 12 ledgers
  behind the current one are dropped (`main.rs:1158-1161`).

### Population sources

- `CacheTxSet` IPC: Core has just built a TX set locally and pushes
  the XDR + hash so the overlay can serve it to peers
  (`main.rs:1067-1093`).
- `TxSetReceived` from libp2p: a peer answered our `fetch_txset`. The
  XDR is stored and forwarded to Core via `TxSetAvailable`.

## Externalization handoff

`TxSetExternalized` IPC (`main.rs:1166-1208`):

1. Core sends `[txset_hash:32][num_tx:4][tx_hash:32]…`.
2. The overlay calls `mempool.remove_txs_sync(tx_hashes)` and **awaits**
   completion before returning to the main event loop. This is
   intentional: the next nomination cycle must not see TXs that have
   already been included.
3. The TX-set entry itself is **not** removed from the cache here — it
   stays around to serve catch-up replies. Cache eviction is handled
   later by the per-ledger `evict_before` on `LedgerClosed`.

## Known gaps

- **No per-fetch timeout / retry.** A retry task was scaffolded in
  `libp2p_overlay.rs:1829-1916` and is **commented out**. If a peer goes
  silent after receiving our 32-byte request, the pending entry
  effectively leaks until the peer disconnects. Core's own retry policy
  is the only safety net.
- **No alternate-peer fallback** within a single fetch. We pick one peer
  and stop.
- **Eviction is non-deterministic** under capacity pressure. Acceptable
  given the `evict_before(seq-12)` cleanup, but worth flagging.
- **No request prioritization**. All TxSet fetches are FIFO on the
  shared TxSet stream.
