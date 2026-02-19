# Experiment 005: Pre-Verified Signature Set

## Status: FAILURE

## Hypothesis
Experiment 004 (batch ed25519 verification) failed because batch-verified results
stored in `RandomEvictionCache` were evicted by subsequent `put(key, false)` calls
during sequential processing. By storing batch-verified signature hashes in a
thread-local `std::unordered_set<Hash>` instead, we can prevent eviction and
successfully skip crypto during the sequential pre-apply loop.

## Implementation
1. Added `batchPreVerifySigs()` function that collects all (pubkey, sig, hash) tuples
   from pending transactions before the sequential loop
2. Verified signatures in parallel using `std::async` with `hardware_concurrency()` threads (32)
3. Stored verified signature cache keys in a thread-local `std::unordered_set<Hash>`
4. Modified `verifySig()` to check the pre-verified set before the sharded cache
5. Added `clearPreVerifiedSigs()` to clean up after the sequential loop

## Results
- **Baseline**: 9,408 TPS
- **Experiment**: 8,896 TPS
- **Change**: -5.4% (REGRESSION)

## Debug Analysis
Debug logging confirmed the approach works correctly:
- `batchPreVerifySigs: 9472 items, 9472 verified, 32 threads`
- `PreVerifiedSigs: 18944 hits, 0 misses, set size 9472`
- Each transaction has exactly 1 signature, `verifySig` called 2× per tx (tx-level + op-level)
- Pre-verified set was hit on every lookup with 0 misses

## Root Cause of Failure
The overhead of batch pre-verification exceeds the savings:

1. **Savings are smaller than expected**: With only 2 verifySig calls per tx and
   ~42µs per crypto op, the first call populates the sharded cache and the second
   hits the cache. The pre-verified set only saves the first cache miss (~42µs × 9,472 txs / ~7 ledgers ≈ ~57ms/ledger), but the sharded cache was already fast.

2. **Thread spawn overhead**: Spawning 32 `std::async` threads per ledger adds
   measurable overhead for the ~50ms of parallelizable crypto work.

3. **Double BLAKE2 computation**: The cache key (`verifySigCacheKey`) must be
   computed both during batch pre-verify and during the sequential `verifySig()`
   lookup, adding redundant work.

4. **Set construction/destruction**: Building and destroying an `unordered_set`
   with 9,472 entries per ledger adds allocation overhead.

## Key Learning
- Each transaction in the benchmark has exactly 1 signature (not 8 as previously assumed)
- `verifySig` is called exactly 2× per transaction (tx-level + op-level validation)
- The sharded cache (Exp 001) already handles the second call efficiently
- Pre-verification only saves the first-call cache miss, which is insufficient
  to overcome the overhead of parallel batch verification
- Signature verification is NOT the bottleneck at this point — it's only ~57ms/ledger
  of crypto work in the sequential loop

## Conclusion
Further optimization of signature verification has diminishing returns. The
sequential pre-apply loop's remaining cost is dominated by `commonValid()` logic
other than crypto (sequence number checks, fee computation, LedgerTxn operations),
and the parallel apply phase (49% of ledger time) where per-tx VM overhead is 20×
the actual SAC transfer work.
