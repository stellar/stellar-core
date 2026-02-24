# Experiment 046: Incremental txSetResultHash Computation

## Date
2026-02-23

## Hypothesis
Instead of building the TransactionResultSet vector with 16K deep copies and
then hashing it with xdrSha256, compute the SHA256 hash incrementally as each
TX is processed in processResultAndMeta. In benchmark mode (no meta, no
history), skip building the vector entirely. Expected savings: ~35ms (33ms
from processResultAndMeta copy + 2ms from xdrSha256).

## Change Summary
- `src/ledger/LedgerManagerImpl.h` — Added XDRSHA256 hasher parameter to
  processResultAndMeta, applyTransactions, applySequentialPhase,
  processPostTxSetApply. Added `bool needsResultVector` to avoid building
  the TransactionResultSet when neither meta nor history is needed.
- `src/ledger/LedgerManagerImpl.cpp` — Created XDRSHA256 hasher in
  applyTransactions with vector length prefix, threaded through all call
  sites. processResultAndMeta hashes incrementally via xdr::archive. Three
  code paths: meta (copy + hash), needsVector (copy + hash), hash-only
  (hash without copy). applyLedger uses pre-computed hash instead of
  calling xdrSha256 on the full vector.

## Results

### TPS
- Baseline: 16,960 TPS [16,960, 17,024]
- Post-change: 16,960 TPS [16,960, 17,024]
- Delta: **0%** (no change)

### Tracy Analysis
- processResultAndMeta self-time: 36.7ms/ledger (was 33.2ms) — **+10% WORSE**
- processPostTxSetApply total: 64.6ms/ledger (was 61.6ms) — **+5% WORSE**
- xdrSha256 txResultSet: eliminated (was 1.7ms)
- Net effect: ~+1.3ms WORSE per ledger

## Why It Failed

The optimization hypothesis was wrong about what dominates the per-TX cost
in processResultAndMeta. The ~2μs per TX is dominated by **cache misses**
(~1000-1500ns) when accessing scattered TX objects in memory, not by the
XDR deep copy itself (~300-500ns including heap allocation).

Incremental SHA256 hashing adds ~200-300ns per TX and destroys the cache
locality benefit of the old approach. The old approach (build contiguous
vector, then hash sequentially) naturally achieves good cache behavior on
the final hash pass because the vector data is contiguous in memory.

The 1.7ms saved from eliminating the separate xdrSha256 call is more than
offset by the per-TX overhead of interleaved hashing.

**Lesson**: When per-item cost is dominated by cache misses from random
memory access, interleaving additional work (even cheap work like SHA256
streaming) makes things worse, not better. Batch-then-process patterns
preserve cache locality.

## Files Changed (REVERTED)
- `src/ledger/LedgerManagerImpl.h` — parameter changes
- `src/ledger/LedgerManagerImpl.cpp` — incremental hashing implementation
