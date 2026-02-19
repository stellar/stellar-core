# Experiment 004: Batch Ed25519 Verification

## Result: FAILURE (9,408 → 8,422 TPS, -10.5%)

## Hypothesis
Use `ed25519-dalek`'s `verify_batch()` for multi-scalar multiplication to verify all
N signatures at once (O(n) point additions vs O(n) separate verifications), then populate
the verifySig cache so that individual `verifySig` calls during `commonValid` become cache
hits instead of redundant crypto operations.

## Implementation
1. Added `verify_ed25519_batch_dalek()` Rust FFI function using `ed25519-dalek`'s batch feature
2. Added `PubKeyUtils::batchVerifySigs()` C++ wrapper that collects all (pubkey, sig, hash)
   tuples and calls batch verification, then populates the sharded verifySig cache
3. Called `batchVerifySigs()` before the sequential `preParallelApply` loop
4. Added `features = ["batch"]` to `ed25519-dalek` in Cargo.toml (pulls in `merlin` and `byteorder`)

## Why It Failed
The batch verification itself worked correctly and populated the cache. However, during
the subsequent sequential `preParallelApply` loop, the cache entries were being **evicted**
before they could be read:

1. **Cache eviction was the root cause**: Each transaction's `verifySig` is called ~8 times
   (once per hint-matching signer). Only 1 of those 8 calls finds the actual signer; the
   other 7 call `put(cacheKey, false)` to cache the negative result. These 7 `put()` calls
   into the `RandomEvictionCache` randomly evict existing entries — including the batch-
   populated `true` entries from the pre-verification phase.

2. **Net effect was purely additive overhead**: The batch verification phase took ~40ms/ledger,
   but saved nothing because cache entries were evicted before use, causing full redundant
   individual verification during `commonValid`.

3. **Confirmed via debug logging**: Immediately after batch population, cache readback
   succeeded. But during the sequential loop, `verifySig` cache lookups missed, proving
   the entries were evicted by intervening `put(key, false)` calls.

## Tracy Data
- Benchmark without Tracy: 8,422 TPS (vs 9,408 TPS baseline) = -10.5% regression
- All 67 `[soroban][tx]` test cases passed (49,254 assertions)

## Potential Fixes (Not Pursued)
- **Check-before-put**: Skip `put(key, false)` if key already exists in cache — would
  require adding `exists()` to RandomEvictionCache that doesn't mutate, or restructuring
  the verifySig loop
- **Separate pre-verified set**: Use a `std::unordered_set` of pre-verified cache keys
  alongside the RandomEvictionCache, checked before crypto verification
- **Increase cache size**: Larger cache reduces eviction probability but doesn't eliminate it

## Key Lesson
The `RandomEvictionCache` is designed for high-throughput with simple random eviction. When
multiple cache entries are written per transaction (8 per tx × 9,400 txns = 75,200 writes
per ledger vs 150,000 cache capacity), batch-populated entries have a ~40% chance of being
evicted before they're read. Pre-population strategies must either (a) prevent eviction of
pre-populated entries or (b) bypass the cache entirely with a separate lookup structure.

## Reverted
All changes reverted with `git checkout -- .`
