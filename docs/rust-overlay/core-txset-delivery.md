# Deliver transaction sets when Core requests them

Rust prefetches transaction sets referenced by SCP envelopes and keeps the
verified bytes in its existing cache. It sends a set to Core after
`RequestTxSet`, which Core issues after registering a pending fetch.

Previously every network receipt was pushed to Core. A racing request could
then trigger another full copy from the cache. Depending on ordering, the
early push or redundant reply was rejected only after IPC decoding and
transaction-set construction.

The App task serializes Core requests and network receipts:

- A prefetch arriving first stays cached; a later request receives it.
- A request arriving first records the hash and newest requested slot. The
  arriving set satisfies that demand once; duplicate arrivals remain cached.
- A locally cached set also satisfies a pending request.
- Every explicit later request is serviced, including after cache eviction.
- Pending metadata expires with the existing twelve-ledger retention window.
  Failed IPC enqueueing retains demand. Successful enqueueing is not a Core
  acknowledgment; an IPC connection failure follows the existing lifecycle.

Network prefetching, peer responses, validation, and wire formats are unchanged.
`TXSET_FROM_CACHE` includes normal prefetch delivery and does not by itself
indicate redundant delivery to Core. Waiting for Core's request can add a local
round trip in exchange for avoiding unsolicited copying and decoding.

Tests use real IPC socket pairs for both arrival orders, duplicate receipts,
repeated explicit requests, unrelated hashes, cache eviction, locally cached
responses, slot expiry, and enqueue failure.
