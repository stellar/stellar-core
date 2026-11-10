# TX set fetching

SCP envelopes reference transaction sets by hash. A round's leader pushes the
set it is about to propose to its peers; a node that does not hold a
referenced set requests its body from a peer. Either way Rust keeps the bodies
in the tx-set cache, and Core receives a set only after it establishes demand
with `RequestTxSet`.

## Wire formats and routes

Requests use a four-byte big-endian frame length followed by a
`StellarMessage::GetTxSet` containing the 32-byte hash. Responses require
`/stellar/txset/zstd/2.0.0` and contain an outer four-byte big-endian frame
length, a four-byte big-endian original XDR length, and exactly one zstd frame.
There is no raw encoding or legacy protocol fallback on this experimental branch.

The decoder bounds both encoded and original sizes, requires one complete zstd
frame and an exact output length, and strict-parses `GeneralizedTransactionSet`.
The response hash is computed from the recovered canonical XDR without
re-encoding. The XDR limit remains 16 MiB minus four bytes; the encoded-frame
limit also permits zstd's worst-case expansion for incompressible input.

Requests share the highest-priority control route with SCP. Responses use the
next-priority tx-set route. All routes share QUIC connection limits.
See [transport](transport.md).

## Encoding ownership

Only nomination leaders construct local proposals. Followers start SCP with a
lazy value supplier and vote for received leader proposals without building a
private set or compressing it. At the trigger a follower pulls its mempool's top
transactions once and verifies their master-key signatures on background
threads, so that validating the leader's set hits the signature cache (cold, it
costs about six times as much). The prewarm stops when validation of a received
set or a local build starts. A node promoted by a
nomination timeout builds only if SCP needs its own value; it rechecks ledger
state and chooses a current close time then. Subsequent rounds reuse that value.
First-round leaders may prepare before the trigger; later leaders do no
speculative preparation. Nomination stops on externalization even for followers
that never constructed a local value.

Ledgers closing on a pre-protocol-20 LCL are the exception. Their legacy
`TransactionSet`s cannot be carried by this overlay, so every validator builds
its own value at the trigger, as before leader-only construction. With an empty
mempool (genesis and the upgrade-only ledgers before protocol 20) the set depends
only on the previous ledger, so each validator already holds the leader's set.

Core sends `CacheTxSet` as soon as a locally constructed proposal's final XDR is
available, before the builder's roundtrip and final validation. Rust eagerly
compresses it at zstd level 1 on a blocking worker while Core continues those
checks. App awaits the prepared representation before processing another IPC
message or application event; the network dispatcher remains independent.
Repeated local publication of an already cached hash reuses its encoding.

A receiving overlay retains the original encoded allocation while decompressing
and strict-parsing on a blocking worker. The cache shares an immutable
`Arc<TxSetData>` containing both original XDR and encoded bytes. Peer responses
borrow those encoded bytes directly, so relays never recompress received sets.
Encoding is complete before cache insertion; sending has no lazy initialization
or compression decision. Core continues receiving uncompressed XDR over IPC.

## Leader push

A round leader does not wait to be asked. For the set it is about to propose,
Core sends `PushTxSet [hash:32][slot:u32 LE]` right after the `CacheTxSet`
that carried the set: from inside the build when the node leads the current
round, after the build for a first-round leader's early (prepared) set that
was capacity limited, and again when a prepared set is taken at the trigger.
App looks the hash up in the cache (a miss is logged and ignored) and the
dispatcher writes the prepared encoding to the tx-set stream of every
connected peer that has not been sent this set. The proposal itself follows
on the control stream once Core has validated and persisted it.

The pushed frame is an ordinary tx-set frame that nobody requested. It
carries no hash and no slot: the receiver decompresses, strict-parses and
hashes it like a response and caches it under the hash it computed. Core is
not told. When an SCP statement names that hash, Core's `RequestTxSet` is a
cache hit (`overlay.txset-push.hit`). A pushed set whose hash no statement
names is never delivered to Core.

Push against request. If the proposal reaches a peer before the pushed set is
complete, the peer requests the set as usual. The leader does not answer that
request (`overlay.txset-push.request-skipped`) if it is the first one from
that peer for a set that is still being written to it, or was written less
than 500 ms ago, on the current connection: the bytes are already on the same
ordered, reliable stream, and the peer's reservation is cleared when they
land. Every other request is answered: a repeated one, one that arrives later
(a peer only asks when it does not hold the set, so it lost or dropped the
pushed copy), and one from a peer the set was not pushed to. A failed push
write answers a skipped request after all, and any connection to the peer
closing drops the record for that peer.

A leader keeps at most one push in flight per peer (`TXSET_PUSH_BUSY` in the
log when a peer is left out). A peer that does not read its tx-set stream is
not sent further sets (it requests them), so it pins at most one set in the
leader's memory; closing a connection does not release that count, only the
end of the write does.

A node that did not receive the push (not connected to the leader, connected
later, or busy with an earlier push) has no cached set and uses the request
path below. A push that the receiver itself dropped (rate limit) or evicted
right away (limits on unclaimed sets) is not recovered that fast: the
receiver's request reaches the leader within the 500 ms and is skipped, so the
set only comes with Core's retry to another peer after one second. The window
helps requests that arrive later (an eviction after some time, a first request
that went to a relay). The push is not relayed: a
node that is not connected to the leader requests the set from the peer that
relayed the proposal, which holds the pushed copy.

Limits on what a peer can make a node decode and keep:

- Rate: every tx-set frame takes a token from a per-peer bucket (burst 8, 4
  per second). A frame that answers a request of this node returns its token,
  so only frames nobody asked for use the allowance. With the bucket empty a
  frame still passes if this node sent that peer a request it has not used up
  (one frame per request, at most 8 saved). Anything else is dropped before
  decompression (`overlay.txset-push.dropped`).
- Space: cached sets that came from a peer and that Core has not asked for
  are limited per peer, oldest dropped first: 4 that the peer pushed and,
  separately, 16 that answered this node's own requests (a peer answers a new
  connection's SCP state request with about 12 slots at once). Together they
  are limited to 128 MiB (the peer holding the most bytes loses its oldest
  set). Evictions count as `overlay.txset-push.evicted`. A set leaves this
  accounting when Core requests or builds it, or when the cache drops it. A
  set is accounted to the peer that delivered it first.
- Not bounded by these: a peer can make a node send it requests, because the
  overlay prefetches any hash named in an SCP envelope before Core checks the
  signature; each such envelope buys one frame over the rate. A peer that
  reconnects starts with a full allowance. Many colluding peers can use the
  shared byte budget to push out a large honest set, and a peer that delivers
  the leader's set first and then pushes four more evicts it (the receiver
  then requests the set).

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
reservation and records fetch latency and the requested slot. `LedgerClosed`
drops reservations older than the retained-slot window (`sequence - 12`).

Rust has no request timer. Core repeats a `RequestTxSet` for a set it has not
received after one second, then every second, logging a warning on every
doubling of the request count (and counting `scp.fetch.txset-retry`).
A request for a hash Core is still waiting for is a retry: it replaces the
reservation and goes to another connected peer, preferring the SCP source only
if it was not the previous target, else a random one. Core stops fetching when
the slots waiting for the set are purged.

## Cache and externalization

The cache holds up to 100 sets in App. Each entry contains its content hash,
canonical XDR, encoded response, and ledger sequence. Capacity eviction uses insertion order;
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

- The push is not relayed and carries the full compressed set; nodes that are
  not connected to the leader, and a compact (hash-list) form, are future work.
- A peer whose first copy of a proposal came through a relay requests the set
  from that relay, which answers from its cache while the leader's push may
  still be in flight: one duplicate transfer.

- A cache miss at the selected peer has no explicit negative response; the
  requester only notices through Core's one-second retry.
- Selecting one connected peer does not guarantee it has the requested body.
- Legacy (pre-protocol 20) `TransactionSet`s cannot be fetched at all; see
  encoding ownership.
