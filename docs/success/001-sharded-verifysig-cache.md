# Experiment 001: Sharded Signature Verification Cache

## Result: SUCCESS — 7,680 → 8,896 TPS (+15.8%)

## Hypothesis

The global `gVerifySigCacheMutex` in `verifySig()` causes contention when 4
parallel threads verify signatures simultaneously. Each call acquires the mutex
twice (once to check cache, once to store result). With 16 shards, each with
its own mutex, contention is reduced by ~16x.

## Changes

### `src/crypto/SecretKey.cpp`
1. **Sharded cache**: Replaced single `std::mutex` + `RandomEvictionCache<Hash, bool>(250K)`
   with `std::array<VerifySigCacheShard, 16>` where each shard has its own mutex
   and cache of size 15,625 (250K/16). Shard selection via `std::hash<Hash>{}(cacheKey) % 16`.

2. **Atomic counters**: Changed `gVerifyCacheHit` and `gVerifyCacheMiss` from
   `uint64_t` (protected by global mutex) to `std::atomic<uint64_t>` with
   relaxed memory order. Also made `gUseRustDalekVerify` atomic.

3. **Single lookup via `maybeGet`**: Replaced `exists()` + `get()` double-lookup
   pattern with single `maybeGet()` call under lock.

4. **String allocation fix**: Replaced heap-allocated `std::string("hit")` and
   `std::string("miss")` for `ZoneText` with string literals.

### `src/ledger/LedgerManagerImpl.cpp`
5. **Removed unused snapshot copy**: Deleted `auto liveSnapshot = app.copySearchableLiveBucketListSnapshot()`
   at line 2321 which was created but never used.

## Tracy Self-Time Comparison (30s trace)

| Zone | Baseline | Experiment 001 | Change |
|------|----------|----------------|--------|
| `verify_ed25519_signature_dalek` | 3.35s | 2.87s | -14.3% |
| `applySorobanStageClustersInParallel` | 4.06s | 4.82s | +18.7% (expected: more TPS = more total work) |

## Files Changed
- `src/crypto/SecretKey.cpp`
- `src/ledger/LedgerManagerImpl.cpp`

## Tracy Profile
- `/mnt/xvdf/tracy/exp001-sharded-cache.tracy`
