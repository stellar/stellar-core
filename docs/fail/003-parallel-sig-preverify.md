# Experiment 003: Parallel Signature Pre-Verification

## Result: FAILURE (9,408 → 9,216 TPS, -2.0%)

## Hypothesis
Move ed25519 signature verification from the sequential `preParallelApply` loop
to a parallel pre-verification phase. Collect all (pubkey, signature, contentsHash)
tuples and verify them in parallel using `std::async` with `hardware_concurrency()`
threads. The cache would be populated before `commonValid` runs, turning expensive
crypto operations into cheap cache hits.

## Implementation
- Added parallel pre-verification block in `preParallelApplyAndCollectModifiedClassicEntries`
  before the sequential `preParallelApply` loop
- Collected all single-signer ed25519 signatures from all transactions
- Split work into chunks across `std::thread::hardware_concurrency()` threads
- Each thread called `PubKeyUtils::verifySig()` which populates the sharded cache

## Why It Failed
The approach is fundamentally break-even with marginal overhead:

1. **Pre-verification took ~37ms/ledger** on the critical path (before the sequential loop)
2. **Sequential loop was faster** because of cache hits, saving ~equivalent time
3. **Net effect ≈ 0**: The work was the same total amount, just rearranged
4. **Thread management overhead** (~7ms/call self-time) made it slightly negative
5. **Parallelism benefit** (~3x speedup on 4 threads) was offset by the fact that
   ed25519 verification was not the dominant cost in the sequential `preParallelApply` —
   it's only ~30% of the sequential loop time, so saving 75% of 30% = ~22% of the
   sequential phase, which is modest

## Tracy Data
- `preVerifySignaturesParallel`: 298ms total / 8 calls = ~37ms/call, self-time 59ms
- `verify_ed25519_signature_dalek`: 62,465 calls / 2.63s (vs 63,745 / 2.70s baseline)
- `applyLedger`: 8,676ms / 7 ledgers = 1,239ms/ledger (vs 1,236ms baseline)

## Key Lesson
Moving sequential work to parallel only helps if the total wall-clock time decreases.
When the parallel phase is on the critical path (blocking before the sequential phase),
the savings must exceed the overhead. A better approach would be to overlap verification
with other work (pipelining) rather than just parallelizing verification alone.

## Reverted
All changes reverted with `git checkout -- src/transactions/ParallelApplyUtils.cpp`.
