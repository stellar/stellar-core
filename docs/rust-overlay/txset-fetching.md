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
see [transport.md](transport.md#frame-formats)). The payload is a
`StellarMessage`, strict-decoded on receipt:

| `StellarMessage` arm | Meaning  | Payload                           |
|----------------------|----------|-----------------------------------|
| `GetTxSet`           | Request  | The 32-byte TX set hash           |
| `GeneralizedTxSet`   | Response | The full `GeneralizedTransactionSet` XDR |
| `DontHave`           | Negative response | `type = GENERALIZED_TX_SET`, the requested hash |

Responses carry no explicit hash: because the decode is strict, the
bytes after the 4-byte union discriminant are the canonical encoding,
and the reader identifies the set by their sha256 — no re-encode. The
inbound handler (`libp2p_overlay.rs:1855`) verifies/clears the pending
request and measures fetch latency right there in the reader task, so
the main loop receives an already-verified `TxSetReceived { hash, data }`.

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

A peer sends a `GetTxSet(hash)` frame on its TxSet stream. The inbound
handler emits a `TxSetRequested { hash, from }` event
(`libp2p_overlay.rs:1882-1899`). The main loop looks up the cache and,
on hit, calls `send_txset_response(peer, hash, xdr)`, which frames the
cached canonical bytes as a `GeneralizedTxSet` message (discriminant
prefix, no re-encode) and sends it on the TxSet stream
(`libp2p_overlay.rs:994-1010`).

On miss the overlay answers `DontHave` (`send_txset_dont_have`), so the
requester retries another peer immediately instead of waiting for its
timeout.

## Fetch lifecycle, peer selection and retry

`start_txset_fetch` / `retry_txset_fetch` in `libp2p_overlay.rs`, with
the bookkeeping in `flood/txset_fetch.rs` (`PendingTxSetFetches`).

Core issues each `RequestTxSet` once (a long-pending set is re-requested
only as a safety net, see below), so the overlay owns seeing a fetch
through. A fetch is *pending* from the first `GetTxSet` until a matching
set arrives or its slot ages out; while pending it is re-dispatched to
another peer whenever the current peer

- answers **`DontHave`** (immediately),
- **disconnects** (immediately), or
- **stays silent** for `TXSET_FETCH_RETRY_TIMEOUT` (2 s), checked by the
  50 ms housekeeping task.

Peer selection (`PendingTxSetFetch::choose_next_peer`):

1. The **SCP source** — `txset_sources` maps `hash → PeerId` for the peer
   whose SCP message referenced the hash — if connected and not yet
   asked.
2. Any connected peer not yet asked.
3. Once everyone has been asked, wrap around to any connected peer other
   than the one currently being waited on (or that peer again if it is
   the only one).

A request to the same hash while a fetch is pending is a no-op (dedup);
with nobody connected nothing is recorded and Core's safety net
re-requests later.

Each pending entry records the peer, slot, first-request time (fetch
latency is measured across retries), last-send time, attempts and the
peers tried. `fetch_txset_retry` and `fetch_txset_dont_have` count
re-dispatches and negative answers.

### Abandonment

On `LedgerClosed { seq }` the overlay drops pending fetches for slots at
least `TXSET_FETCH_MAX_SLOT_LAG` (12) ledgers behind `seq` — the same
window after which every peer has evicted the set from its cache, so
retrying is pointless.

### Core-side safety net

`PendingEnvelopes::startFetch` re-issues `RequestTxSet` when an envelope
arrives for a set that has been pending for at least
`TX_SET_REFETCH_INTERVAL` (5 s). This covers the two cases the overlay
cannot: it had no peer to ask when Core first requested (nothing was
recorded), or it abandoned the fetch because the slot aged out. While the
overlay does have the fetch pending, the re-request is deduplicated.

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
  the XDR + hash so the overlay can serve it to peers. Core is trusted
  for encoding, so the overlay does not decode — it only verifies that
  the hash matches the bytes (`tx_set_hash_matches`, `main.rs:1059`),
  guarding against a mismatch that would make the set unfetchable
  network-wide.
- `TxSetReceived` from libp2p: a peer answered our `fetch_txset`. The
  bytes were strict-decoded and content-hashed in the reader task, so
  they are cached and forwarded to Core via `TxSetAvailable` as-is.

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

- **A timed-out transfer is not cancelled.** If a slow peer is still
  streaming a large set when the retry timeout fires, the set may arrive
  twice (the second copy is pushed to Core, which ignores a set it is not
  waiting for).
- **No request prioritization**. All TxSet fetches are FIFO on the
  shared TxSet stream.
