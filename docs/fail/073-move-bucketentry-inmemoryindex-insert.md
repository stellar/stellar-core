# Experiment 073: Move BucketEntry into InMemoryBucketState::insert

## Status: FAILED (-3.0% regression)

## Hypothesis
`InMemoryBucketState::insert()` calls `make_shared<BucketEntry const>(be)` which deep-copies every BucketEntry during InMemoryIndex construction from file (~80ms sequential phase). By adding a move overload `insert(BucketEntry&&)` and using `std::move(be)` in the file-based constructor loop (where `be` is immediately overwritten on the next iteration), we can avoid the deep copy and save the XDR data allocation/copy per entry.

## Changes
- Added `InMemoryBucketState::insert(BucketEntry&& be)` overload in `InMemoryIndex.h` and `InMemoryIndex.cpp`
- Added move-accepting `processEntry(BucketEntry&&, ...)` overload (anonymous namespace helper)
- Modified file-based `InMemoryIndex` constructor to call `processEntry(std::move(be), ...)` instead of `processEntry(be, ...)`
- Vector-based constructor unchanged (entries come from const ref)

## Files Modified
- `src/bucket/InMemoryIndex.h`: Added `void insert(BucketEntry&& be)` declaration
- `src/bucket/InMemoryIndex.cpp`: Added move overloads for both `processEntry` and `insert`, modified file-based constructor

## Results
- **Baseline**: 19,520 TPS
- **After change**: 18,944 TPS
- **Delta**: -576 TPS (-3.0%)
- **Verdict**: FAILED

## Tracy Profile Analysis (073 vs baseline)
| Metric | Baseline (070b) | Experiment 073 |
|--------|-----------------|----------------|
| InMemoryIndex (5 calls) | ~80ms | 455ms (91ms avg) |
| readOne | ~395ms | 334ms |
| scan | ~324ms | 323ms |
| Total TPS | 19,520 | 18,944 |

The InMemoryIndex total went UP from ~80ms to 455ms, which is suspicious and suggests the profiling context changed (different ledger count sampled, different bucket sizes). The per-call average of ~91ms is similar to baseline.

## Analysis
The optimization was logically sound but had no measurable positive impact. Possible reasons:
1. BucketEntry move semantics may not be significantly cheaper than copy for the entry types in the benchmark (SAC balance entries are small XDR structures)
2. The InMemoryIndex construction is only ~80ms out of ~400ms sequential phase — even a 30% improvement would only save ~24ms
3. The -3.0% regression is likely measurement noise (same pattern as experiments 069-072)

## Conclusion
InMemoryIndex construction is not a bottleneck worth optimizing at this scale. The deep copy of BucketEntry for small entries (SAC balances) is cheap enough that move semantics don't help measurably.
