# Sharded level-0 curr bucket (experimental, consensus-changing)

## Motivation

On the SAC-payment apply-load benchmark (6000 tx/ledger, 16 clusters), the
`seal_and_bucket` close phase costs ~46ms. Instrumentation (see
`LedgerClosePhaseTimings` seal sub-timings) shows the cost is almost entirely
serial CPU on the apply thread, not disk:

| step                                | mean ms |
|-------------------------------------|---------|
| get_all_entries (ltx extraction)    | 5.8     |
| bl_convert_sort (fresh batch sort)  | 13.7    |
| bl_merge (in-memory curr+fresh)     | 5.8     |
| bl_put_serialize (XDR + SHA256)     | 11.0    |
| bl_close_fsync (file write + fsync) | 2.1     |
| bl_index_wait                       | 3.6     |

The parallel apply threads finish with disjoint write sets ~80ms before any
of this starts; all of the sort/serialize/hash work can be done per-thread,
overlapped with apply, if the level-0 curr bucket is allowed to be a *set of
shard files* instead of a single file.

## Design decision: canonical shards (option B)

Two designs were considered:

- **A: logically-single bucket.** Shard the disk writes but keep today's
  bucket hash by re-merging + re-serializing + re-hashing the whole curr in
  memory each ledger. This keeps archives/protocol unchanged but leaves the
  merge (5.8ms) + serialize/hash (11ms) + index (3.6ms) serial work on the
  critical path — roughly half the achievable win.
- **B: canonical shards.** The shard set *is* the level-0 curr bucket. Its
  hash is a hash of shard hashes; no global sort, merge, serialization or
  hashing happens on the critical path at all.

Measured estimate: A leaves ~15ms/ledger (~7% of close time) on the table vs
B. That is not a small impact, so per the project decision rule we take B:
**the bucket list hash changes, and the HistoryArchiveState format and
archive contents change**. This branch is experimental; no backwards
compatibility is kept.

## Representation

- A **shard** is a perfectly ordinary `LiveBucket`: a sorted, hashed,
  indexed XDR file in the bucket dir (plus retained in-memory entries while
  recent). All existing per-file machinery (verify, download, GC, scans,
  iterators, applicators) works on shards untouched.
- The **level-0 curr and snap** of the live bucket list are *composite*
  buckets: a `LiveBucket` holding an ordered list of shards
  (oldest → newest) and no file of its own.
  - `hash(composite) = SHA256(shard0.hash || shard1.hash || ...)`;
    a composite with zero shards has the zero hash (same as an empty
    bucket).
  - Level/bucket-list/header hashing above this is structurally unchanged.
  - File-access APIs (`getFilename`, input iterators) assert on composites;
    every consumer that can see level-0 expands to shards instead.
- Other levels, and the hot archive bucket list, are unchanged: the level-1
  merge that consumes a (sharded) level-0 snap produces a normal single
  bucket file — this is where shards get condensed.

## Per-ledger flow

Within a ledger, level-0 receives one *shard list* instead of one fresh
batch. Shard order (consensus-relevant, derived from the deterministic tx
set structure): `[stage0.cluster0 .. stageN.clusterK, ttl, residual]`,
appended after any shards already in curr from the previous ledger. Empty
shards are skipped.

1. **Thread shards (optimistic writes).** When an apply thread finishes its
   cluster (`applyThread`), its dirty `CONTRACT_DATA`/`CONTRACT_CODE`
   entries are final — cluster RW footprints are disjoint within a stage and
   later stages shadow earlier ones via merge recency. The worker converts
   them to `BucketEntry`s (INIT/LIVE/DEAD from `mIsNew` + presence; a key
   created *and* deleted in the same ledger is skipped, matching ltx
   annihilation), sorts, and hands them to the shard writer, which writes,
   hashes, indexes and adopts the file off the critical path. TTL entries
   are excluded — they need cross-thread reconciliation.
2. **TTL shard.** After the last stage's `commitChangesFromThreads` (which
   max-merges read-only TTL bumps across threads), the dirty TTL entries
   are extracted from the global entry map and written as one shard.
3. **Residual (classic) shard.** At seal, after fee refunds
   (`processPostTxSetApply`) and eviction have landed in the ltx,
   `getAllEntries` runs as today. Every entry whose key was *not* covered by
   an earlier shard goes into the residual shard: fee-source accounts,
   eviction deletions, config upgrades, and everything from non-parallel
   code paths. Non-parallel ledgers (classic-only, old protocols, tests)
   produce a single residual shard — the representation is uniform.
4. **addLiveBatch** waits for the shard futures (normally already complete),
   appends them to the level-0 curr composite, recomputes the (cheap)
   combined hash, and handles spills as before.

A debug/verification config flag re-checks that covered keys' shard values
match the ltx values at seal.

## Merge / spill (condensation)

When level 0 spills, the composite curr becomes snap, and the level-1
`FutureBucket` merge consumes it through a `ShardedMergeInput`: a k-way
walk over the shards (in-memory entries when present, file iterators after
a restart) that resolves equal keys across shards oldest → newest using the
existing `mergeCasesWithEqualKeys` lifecycle rules (INIT+DEAD annihilation,
INIT+LIVE promotion, newest-wins), then merges pairwise against the level-1
curr. The output is a normal single bucket file. The lifecycle algebra is
associative, so this is equivalent to the sequential per-ledger merges done
today.

## History, catchup, persistence

- **HistoryArchiveState**: level-0 `curr`/`snap` additionally carry shard
  hash lists (`currShards`/`snapShards`); the `curr`/`snap` fields hold the
  composite hash. `getBucketListHash`, `differingBuckets` and
  `allBucketHashes` account for shards. Archives store shard files like any
  other bucket; download + per-file SHA256 verification is unchanged.
- **assumeState** reconstructs composites from the shard lists (each shard
  is on disk, indexed on load).
- **FutureBucket** serialization stores the snap-side shard hash list when
  the input was sharded so in-flight level-1 merges restart correctly.
- **GC** (`forgetUnreferencedBuckets`/`cleanupStaleFiles`) treats shard
  hashes as referenced while their composite is referenced.

## Empty ledgers

The legacy per-ledger level-0 merge re-stamped curr with the current
protocol's METAENTRY even when the batch was empty. To preserve the
"curr's version is never below lower levels" invariant relied on by HAS
validation, a ledger that contributes no shards at a META-supporting
protocol adds a meta-only shard (an empty bucket file with just the
METAENTRY). Pre-METAENTRY protocols leave curr genuinely empty, as before.

## Downstream parallel unlocks

With shards canonical, the serial post-apply funnel that existed only to
push soroban entries through the LedgerTxn was dismantled:

- **Early in-memory state update**: the shards retain their entries in
  memory, so `InMemorySorobanState` is updated from them on a background
  thread starting as soon as the last parallel stage commits (TTL changes
  are fed directly, without waiting for the TTL shard's file write). A
  parallel "byproduct scan" produces the modified-soroban-key set (for
  eviction) and new contract code (for the module cache). At seal only the
  eviction-deletion delta and the ledger-seq advance remain
  (`finalizeUpdate`). The state-size snapshot uses the size captured before
  the update started, preserving its as-of-previous-ledger semantics; the
  (rare) config-upgrade path waits for the update before recomputing entry
  sizes.
- **Ltx bypass** (`bypassLtxForSorobanEntries`, enabled when no invariants
  are configured — the production default): CONTRACT_DATA/CODE entries are
  not merged into the global map for the last stage, and no soroban-typed
  entry is written to the LedgerTxn at all. They persist exclusively via
  the shards and the in-memory state. Per-tx meta is unaffected (built from
  per-tx TxEffects during apply — covered by the "soroban state bypasses
  ltx with meta emission" test); eviction's modified-key checks use the
  shard-derived key set; the LedgerTxnRoot entry cache is cleared on every
  commit, so nothing stale survives; upgrades (which may read e.g. the
  ConfigUpgradeSet contract data written in the same ledger) wait for the
  early update first. With invariants configured, everything falls back to
  the legacy full-ltx path.
- The seal-time residual shard is split into up to 4 contiguous chunks
  written in parallel.

## Results (SAC benchmark, 6000 tx/ledger, 16 clusters, 200 ledgers)

| metric              | before  | after   |
|---------------------|---------|---------|
| mean close          | 227.5ms | 173.1ms |
| p50 close           | 226.2ms | 172.2ms |
| p95 close           | 248.4ms | 179.3ms |
| p99 close           | 256.9ms | 194.9ms |
| seal_and_bucket     | 48.2ms  | ~8ms    |
| commit_to_ltx       | 20.7ms  | ~4.9ms  |
| commit_from_thrds   | 17.1ms  | ~14.2ms |
| get_all_entries     | (in seal) | ~1.0ms |
| sql_commit          | 11.0ms  | ~3.4ms  |

Total: −54.4ms mean close (−24%). Remaining candidates: commit_from_thrds
(~14ms, TTL reconciliation merge), soroban_setup_glbl (~33ms, pre-apply),
process_fees_seqnums (~14ms, serial classic fee processing).

## Queries

- Lookups iterate shards newest → oldest inside the level-0 slots of
  `loopAllBuckets` (snapshot data expansion). Shards built in-process keep
  in-memory entries and use `InMemoryIndex`, so apply-path account loads
  never touch disk; after a restart shards are file-backed and indexed like
  any small bucket.
- Soroban entry types (CONTRACT_DATA/CODE/TTL) are never point-queried from
  the bucket list during apply (`InMemorySorobanState` is authoritative), so
  per-shard lookup fan-out is irrelevant for them.
- The eviction scan never touches level 0 (`startingEvictionScanLevel >= 1`
  is enforced), so `EvictionIterator` semantics are unaffected.
