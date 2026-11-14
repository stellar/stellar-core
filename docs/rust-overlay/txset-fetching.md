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
closing drops the record for that peer. (If a connection closes while a push
is still being written, that push is no longer recorded: a request that
crosses it gets a second copy, and if the write then fails, a request that
was skipped before the close is not answered and falls to Core's retry.)

A leader keeps at most one push in flight per peer (`TXSET_PUSH_BUSY` in the
log when a peer is left out). A peer that does not read its tx-set stream is
not sent further sets (it requests them), so it pins at most one pushed set
in the leader's memory; closing a connection does not release that count,
only the end of the write does. Answers to that peer's requests queue behind
the stuck write under the send permits that bound all tx-set responses.

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
  per second). A frame that delivers a set this node has a request pending
  for returns its token (to the peer that delivered it, whichever peer was
  asked), so only frames nobody is waiting for use the allowance. With the bucket empty a
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
  then requests the set). The cache itself still holds 100 sets of any kind,
  first in, first out: with up to 20 unclaimed sets per peer, five peers can
  fill it and cycle out sets the node built or already uses (on the base one
  peer could, through prefetches).

## Spanning tree for the push (experimental)

`EXPERIMENTAL_TX_SET_TREE` (`off`, `star`, `hash`, `rtt`, `coded`; default
`off`) changes who the leader's set travels through. With `off` nothing below
applies. Every node of a network should use the same value. Code:
`overlay/src/flood/txset_tree.rs`, and for `coded`
`overlay/src/flood/txset_coded.rs` (see "Coded pieces" below).

The tree. Its nodes are the nodes of the leader schedule. For a slot and a
leader the leader is the root and the other nodes follow in the order of
`SHA256(seed || leader || node)`, where `seed` is the seed of the slot's leader
schedule. Position `i` has the children `F*i+1 .. F*i+F`: the complete F-ary
tree, the least depth a fanout of F allows. F is
`EXPERIMENTAL_TX_SET_TREE_FANOUT`; 0 (the default) is the smallest F that
reaches every node in two hops (9 for 90 nodes). Every node derives the same
tree from what it already agrees on; nothing about it is negotiated.

What Core tells the overlay. At every ledger close Core sends
`SetTxSetTree [mode:1][fanout:1][slots:1]([slot:u32 LE][seed:32][leaders:1][nodeID:32]...)...[count:u32 LE][nodeID:32]...`
for the next two slots: the seed of each, the nodes that may lead it (the
leaders of the next slot's first three rounds; for the slot after it those and
the first three of a new order) and the nodes of the trees. Two slots, because
a leader may push the set of slot N+1 the moment it closes N, when a follower
has only closed N-1. The top bit of the mode byte tells an overlay to pass
nothing on (`EXPERIMENTAL_TX_SET_TREE_WITHHOLDERS_FOR_TESTING = N`: the N
validators with the smallest node IDs; for tests and fault runs).

On the wire. A tree push is the ordinary tx-set frame with a header in front,
on the same stream: `[tag:u32 = 0xFFFFFFF1][kind:1][fanout:1][hop:1][0][slot:u32]`
`[leader:32][sent:u64 Unix ms][n:u16][n x u16 order][8-byte digest if n > 0]`
(big-endian). The tag stands where an ordinary frame has the length of its
XDR, which is at most 16 MiB, so no ordinary frame starts with it. Kinds: 1
star (to every peer, never relayed), 2 hash (the tree above), 3 explicit (the
order is in the header, see `rtt`).

Pushing. `PushTxSet` now makes the leader's overlay write the set to its
children in the slot's tree. If it does not know the slot's tree, or a child
has no identified connection, it writes a star to every peer instead. If a
child is left out because an earlier push to it is still in flight, the nodes
below that child get a star from the leader directly.

Relaying. A node that reads a tagged frame passes the bytes on to its children
before it decompresses or checks them, if all of these hold: its mode is
`hash` or `rtt`; its Core described the tree of the frame's slot; the frame's
leader is one of the leaders Core named for that slot; the sender is its
parent in that tree, by the validator identity of the connection; the body is
of a size `decode` would accept; and it has relayed fewer than two frames of
that slot and leader and not this one. One relay is in flight per child. The
node then verifies and files the set as it does any other, by the hash of its
content.

Asking. A node of the tree that does not hold a set a proposal names waits
300 ms from the first mention of the hash before it asks a peer (Core's own
one-second retry is not delayed). Without that wait every node the leader did
not write to would ask it when the proposal arrives.

`star` is the push as it was, with the header (so that the delay from the
leader's send is recorded like a tree's). `rtt`: every node measures the round
trip to each identified peer every two seconds with a 13-byte probe on the
tx-set stream (`[tag:u32 = 0xFFFFFFF2][1 ping | 2 pong][u64]`; the answer must
match the probe sent last); the leader groups its peers by the gaps in the
sorted times, gives every group relays in proportion to its size and fills a
relay's children from its own group, and sends that order in the header. A
receiver follows such an order only in `rtt` mode, only if it has two levels,
and only if it refers to the same list of nodes.

### Coded pieces (`coded`)

In a tree a node has one path to the set: its parent. In `coded` mode the
leader cuts the frame it would push into k pieces, adds m-k Reed-Solomon
parity pieces (any k of the m rebuild the frame), and every node gets its m
pieces from m different senders. Up to m-k of a node's senders can be down,
slow or hostile without the node having to wait and ask.

The layout is derived like the tree. `others` is the hash order of the slot and
leader without the leader. From the number of other nodes alone: m = min(16,
others), k = ceil(m/2), r = others / m relays per piece (90 validators: 16, 8
and 5; fewer than 4 others: no coding, the set is pushed to everyone). Piece
j's relays are `others[j*r .. j*r+r-1]`; the leader writes them piece j. The
node at position p gets piece j from relay `(p + j) mod r` of that piece,
unless it is one of the piece's relays. No piece makes more than two hops. The
leader writes m*r pieces of a k-th of the frame each (10 frame sizes at 90
validators), a relay about (others - r) / r of them (2.1 frame sizes); the
network as a whole writes m/k times the frame per node.

A piece frame, on the tx-set stream (big-endian):
`[tag:u32 = 0xFFFFFFF3][version:1 = 1][hop:1][k:1][m:1][index:1][path length:1][0][0]`
`[slot:u32][leader:32][sent:u64 Unix ms][frame length:u32][root:32]`
`[overlay key:32][binding signature:64][root signature:64][path: 32 each][piece]`.
The leaves of the Merkle tree are `SHA256(0x00 || index || piece)`, inner nodes
`SHA256(0x01 || left || right)`, padded with zero leaves to a power of two. The
root signature is the leader's overlay (libp2p Ed25519) key over
`SHA256("stellar-overlay tx set pieces v1" || network || slot || leader || k || m || frame length || root)`;
the binding signature is the validator's over the peer id of that overlay key,
as nodes exchange it as their identity. So a piece can be checked against the
leader's node ID alone, without a connection to the leader.

A node looks at a piece only if its mode is `coded`, its Core described the
slot and named the leader as one that may lead it, k and m are what the node
derives itself, the sender is the node that owes it that index in the layout
(by the validator identity of the connection) with the hop count that goes
with it, and the sender has sent fewer than 6 piece frames for that slot and
leader. It keeps the piece only if both signatures and the Merkle path check
(the signatures once per root); a leader has room for two roots per slot. A
relay passes its piece on after those checks, once per root, and only if the
leader itself sent it. Pieces do not draw on the per-peer allowance for
unrequested frames: what they can make a node keep and rebuild is bounded by
the above, and nothing is decompressed before k of the node's senders agree on
a root the leader signed.

With k pieces under one root the frame is rebuilt (no decoding if they are
the k data pieces) and goes the way of every received frame: decompress,
strict parse, content hash, cache. Afterwards the node encodes the frame again
and compares the root: a leader whose pieces do not fit together is counted
(`sets-inconsistent`); the set is still what its content hash says. A node
waits for pieces as it does for a tree frame (300 ms) before it asks.

What the code protects is the delivery. A set is identified by the hash of
its content, as before; a wrong piece, root or rebuild gives bytes that fail
the decompression or the parse, or a set with another hash that no SCP
statement names.

Metrics: `overlay.txset-coded.*` (sets cut into pieces; pieces sent, relayed,
received, late, refused, invalid, duplicate, skipped; sets rebuilt, rebuilt
with parity, failed, inconsistent; microseconds of rebuilding) and
`overlay.txset-tree.withheld`. The leader's piece bytes count in
`overlay.txset-push.sent-bytes`, a relay's in `overlay.txset-tree.relayed-bytes`,
and a rebuilt set in `overlay.txset-tree.received` and the delay metrics (at
the arrival of the k-th piece).

Metrics: `overlay.txset-tree.*` (copies written as root and as relay, frames
received, refused, duplicates, relays over the limit, busy and unreachable
children, requests put off and those sent after the wait, the delay since the
leader's send in total, per hop count and in ranges) and
`overlay.txset-push.sent-bytes`.

Limits of the prototype:

- No version negotiation: a node without this change reads a tagged frame or a
  probe as a malformed set.
- In `hash` and `rtt` a relay that is down, slow, two ledgers behind or has
  another view of the nodes costs the nodes below it the 300 ms wait: there is
  no second path. `coded` is the mode with several paths.
- `coded` writes twice the bytes of a set per node (m/k), and its layout is by
  hash only: it does not use round-trip times.
- A piece a relay could not write (the receiver has four in flight, or no
  identified connection) is not sent later; the code absorbs the gap.
- The delay metric compares two hosts' clocks and only counts tagged frames
  that arrived.
- A leader of a fourth or later round is not one of the named leaders: its set
  is not relayed, and the nodes ask for it.
- Relays have no write timeout (neither has the leader's push).

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

- Without the tree the push is not relayed; with or without it, it carries the
  full compressed set. Nodes that are not connected to the leader (or, in a
  tree, to their parent), and a compact (hash-list) form, are future work.
- A peer whose first copy of a proposal came through a relay requests the set
  from that relay, which answers from its cache while the leader's push may
  still be in flight: one duplicate transfer.

- A cache miss at the selected peer has no explicit negative response; the
  requester only notices through Core's one-second retry.
- Selecting one connected peer does not guarantee it has the requested body.
- Legacy (pre-protocol 20) `TransactionSet`s cannot be fetched at all; see
  encoding ownership.
