# Experiment 065: Skip File I/O in mergeInMemory When allBucketsInMemory

## Date
2026-02-24

## Hypothesis
When allBucketsInMemory() is true, mergeInMemory can compute the bucket hash
without writing to a file, saving ~14ms/ledger from the 34ms put loop in the
addLiveBatch path. This should give ~1-2% TPS improvement.

## Change Summary
- Added `hashBucketEntryXDR` helper to hash bucket entries without file I/O
- Added `allBucketsInMemory()` branch in `mergeInMemory` to create file-less
  in-memory-only buckets (empty filename, non-zero hash)
- Added `registerInMemoryBucket` to BucketManager for bucket map registration
- Modified `BucketBase::isEmpty()` to allow empty filename with non-zero hash
- Modified `BucketBase::getIndex()` assertion
- Modified `checkForMissingBucketsFiles` to skip in allBucketsInMemory mode

## Results
FAILED — multiple cascading issues:

1. **isEmpty() assertion**: The existing `isEmpty()` enforces filename↔hash
   consistency. In-memory-only buckets violate this invariant.

2. **getIndex() assertion**: Similar filename-based assertion.

3. **checkForMissingBucketsFiles**: Checked bucket files on disk, failing for
   file-less buckets.

4. **assumeState bucket lookup**: `getBucketByHash` couldn't find buckets not
   registered in the bucket map.

5. **BucketList snapshot lookup failure**: Even after fixing assertions 1-4,
   the BucketList snapshot couldn't load CONFIG_SETTING entries from file-less
   buckets. The entire BucketList lookup infrastructure assumes buckets have
   corresponding files for entry scanning and index building.

## Why It Failed
The bucket infrastructure deeply assumes that non-empty buckets have
corresponding files on disk. File-less buckets break:
- `isEmpty()` invariants
- `BucketInputIterator` (reads from file)
- `IndexBucketsWork` (indexes from file)
- `BucketListSnapshot` lookups (uses file-based index/scan)
- `checkForMissingBucketsFiles` verification

The optimization would require a pervasive refactoring of the bucket
infrastructure to support file-less buckets as first-class citizens.
The savings (~14ms or ~1.5% of apply time) don't justify that scope.

## Files Changed (reverted)
- `src/bucket/LiveBucket.cpp`
- `src/bucket/BucketBase.cpp`
- `src/bucket/BucketManager.cpp`
- `src/bucket/BucketManager.h`
