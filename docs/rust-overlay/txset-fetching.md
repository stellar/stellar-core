# TX set fetching

SCP envelopes reference transaction sets by hash. Rust prefetches their bodies
from peers and keeps them in the tx-set cache. Core receives a set only after
it establishes demand with `RequestTxSet`.

## Wire formats and routes

Network messages use a four-byte big-endian frame length followed by a
`StellarMessage`. `GetTxSet` contains the 32-byte hash; `GeneralizedTxSet`
contains the set XDR. The response hash is computed from the canonical bytes
after strict decoding, without re-encoding the set.

Requests share the highest-priority control route with SCP. Responses use the
next-priority tx-set route. Legacy-only peers use separate request and response
streams under the old tx-set protocol, while SCP uses its old protocol. All
routes share QUIC connection limits. See [transport](transport.md).

## Core requests and network prefetches

`RequestTxSet` IPC carries `[hash:32][slot:u32 LE]`. App records the newest
requested slot for each pending hash. A cache hit immediately satisfies that
demand. A miss initiates a network fetch; the eventual matching arrival is
cached and satisfies the pending demand once. Duplicate arrivals remain cached.

A network prefetch arriving before the Core request stays in the cache. A later
explicit request still receives it, even if Core has requested the same hash
before. Locally built sets supplied with `CacheTxSet` can also satisfy pending
demand. There is no permanent delivered marker. Failed IPC enqueueing retains
the pending request.

## Peer requests

The reader emits `TxSetRequested { hash, from }`. App looks up the cache and
starts a response send. Send admission and the stream write happen outside the
App and network dispatcher loops. Count and byte permits bound admitted bulk
sends; per-route stream locks preserve complete frame ordering.

A cache miss has no network reply. There is no `DontHave` message or automatic
alternate-peer retry in this path.

## Fetch selection

Before spawning the request write, the dispatcher reserves the hash with
`(peer, request_time, slot)` in `pending_txset_requests`. An existing request to
a connected peer suppresses another request. Selection prefers the connected
peer that supplied an SCP reference, then another connected peer. With no peer,
the attempt returns without reserving the hash.

A failed write removes only its own reservation. Disconnect cleanup removes
reservations assigned to that peer. A received response clears its matching
reservation and records fetch latency and the requested slot. This does not
provide a timeout or automatic retry for a silent peer.

## Cache and externalization

The cache holds up to 100 sets in App. Each entry contains its content hash,
canonical XDR, and ledger sequence. Capacity eviction uses insertion order;
updating an existing hash does not move it. `LedgerClosed` evicts entries older
than `sequence - 12`, using saturating subtraction. Pending Core demand expires
against the same retained-slot boundary.

`CacheTxSet` IPC carries `[hash:32][slot:u32 LE][txset_xdr]`. Core's bytes are
trusted for encoding, but their content hash is checked before insertion.
Network bytes are strict-decoded and content-hashed by the reader before App
caches them. Unrequested arrivals are not sent to Core.

`TxSetExternalized` supplies the set hash and included transaction hashes.
App awaits mempool removal before processing the next Core message. The cached
set remains available for peer requests until normal capacity or slot eviction.
See [mempool](mempool.md).

## Remaining limitations

- A peer that stays connected but silent can leave a fetch pending indefinitely.
- A cache miss at the selected peer has no explicit negative response or retry.
- Selecting one connected peer does not guarantee it has the requested body.
