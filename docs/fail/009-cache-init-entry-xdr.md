# Experiment 009: Cache Initial Entry XDR Info to Eliminate Redundant Serializations

## Status: FAILED (+1.4% improvement — marginal, not significant)

## Hypothesis
In `invoke_host_function` (enforcing mode), the function `get_ledger_changes()`
re-serializes ALL ledger keys and old entries to XDR, even though these same
entries were just decoded FROM XDR in `build_storage_map_from_xdr_ledger_entries`.
Additionally, the entire `StorageMap` is `metered_clone`d before execution just
to provide a snapshot for diffing afterward. By caching the initial entry info
(encoded keys, old entry sizes for rent) during construction and passing it to
`get_ledger_changes`, we can eliminate:

1. Re-serialization of all ledger keys (~`metered_write_xdr` per key)
2. Re-serialization of old entries for rent size computation
3. The expensive `metered_clone` of the entire `StorageMap`

Oracle estimated +6-12% TPS improvement (9,408 → ~10.0K-10.5K).

## Implementation

### Core changes to `e2e_invoke.rs` (p25):

1. **`InitEntryCacheEntry` struct and `InitEntryCache` type** (new):
   - Stores pre-encoded key bytes, old entry size for rent, and old
     live-until-ledger for each entry.
   - `InitEntryCache` is a `HashMap<LedgerKey, InitEntryCacheEntry>`.

2. **`build_storage_map_from_xdr_ledger_entries`** (modified):
   - Returns 3-tuple `(StorageMap, TtlEntryMap, InitEntryCache)`.
   - During entry decoding, captures encoded key bytes and computes
     `entry_size_for_rent` before discarding the XDR.
   - Cache construction is `cfg`-gated: only built when NOT in recording mode
     (`is_recording_mode == false`).

3. **`get_ledger_changes`** (modified, enforcing mode only):
   - Accepts `InitEntryCache` instead of `SnapshotSource`.
   - Uses cached encoded keys (with `budget.charge(ValSer, ...)` for metering
     equivalence) instead of re-serializing.
   - Uses cached `old_entry_size_for_rent` and `old_live_until_ledger` instead
     of looking up old entries from snapshot and re-serializing them.
   - Still serializes NEW entries (these change after execution).

4. **`invoke_host_function`** (modified, enforcing mode):
   - Removed `metered_clone` of `StorageMap` (no longer needed since we don't
     diff against a snapshot).
   - Passes `init_entry_cache` to `get_ledger_changes`.

5. **`get_ledger_changes_recording`** (new, recording mode only):
   - Exact copy of the original `get_ledger_changes` logic for recording mode.
   - Uses `&*ledger_snapshot` (the original `Rc<dyn SnapshotSource>`)
     to preserve exact budget metering equivalence.

### Files Modified
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs`

## Results
- **Baseline**: 9,408 TPS [9,408, 9,472]
- **Experiment run 1**: 9,536 TPS [9,536, 9,664]
- **Experiment run 2**: 9,536 TPS [9,536, 9,664]
- **Change**: +128 TPS (+1.4%)
- **Tracy trace**: `/mnt/xvdf/tracy/exp009-cache.tracy`
- **Baseline trace**: `/mnt/xvdf/tracy/exp002-commit-opt.tracy`
- **Tests**: All [tx] tests pass (521 p24 + 588 p25), all Rust soroban tests pass

### Tracy Analysis (self-time comparison, per-tx normalized)

| Zone | Baseline (exp002) | Exp009 | Delta |
|------|-------------------|--------|-------|
| `write xdr` calls | 513,423 | 562,183 | +9.5% (more txs) |
| `write xdr` per-call | 2,943ns | 3,378ns | +14.8% |
| `invoke_host_function` per-tx | 22,177ns | 24,350ns | +9.8% |
| `map lookup` per-call | 412ns | 477ns | +15.8% |
| `new map` per-call | 652ns | 738ns | +13.2% |
| `charge` per-call | 45ns | 44ns | -2.2% |

The per-call times for many zones regressed slightly, consistent with icache
layout perturbation from the code changes (same pattern as experiments 006-008).

## Why It Failed

1. **Budget charging overhead dominates**: The optimization replaces
   `metered_write_xdr(key)` with `budget.charge(ValSer, len)` + `vec.clone()`.
   The `budget.charge()` call itself costs ~45ns (from Tracy), and a vec clone
   of ~100-200 bytes costs ~50-100ns. The original `metered_write_xdr` for a
   key costs roughly the same: budget charge + XDR serialization of a small key.
   The savings from avoiding XDR serialization are negligible because keys are
   small (100-200 bytes) and XDR serialization of small structs is fast.

2. **Old entry serialization savings are real but small**: Avoiding the
   `metered_write_xdr` of old entries saves one serialization per entry, but
   these entries are also small for SAC transfers (ContractData entries with
   i128 balances). The `entry_size_for_rent` call is trivial after that.

3. **`metered_clone` was not the bottleneck**: The StorageMap `metered_clone`
   was not instrumented in Tracy, so we assumed it was expensive. In practice,
   for SAC transfers, each StorageMap contains only ~5-8 entries per TX
   (2 balance entries + contract instance + contract code + TTL entries).
   Cloning 5-8 `Rc<LedgerEntry>` entries is very cheap (just reference count
   bumps + metered map structure copy).

4. **Cache lookup overhead**: The `HashMap<LedgerKey, InitEntryCacheEntry>`
   lookup involves hashing a `LedgerKey` (which requires traversing its XDR
   structure), partially offsetting the avoided serialization.

5. **The real per-tx bottleneck is elsewhere**: Tracy shows the dominant
   self-time zones are `charge` (2.5B ns), `verify_ed25519_signature_dalek`
   (2.8B ns), `write xdr` (1.9B ns), and `visit host object` (2.0B ns).
   The `get_ledger_changes` portion (key serialization + old entry lookup)
   accounts for only ~5-10µs per TX out of ~250µs total — a ~2-4% slice.
   Even eliminating it entirely would yield only 2-4% TPS improvement.

## Key Learning
- **Small-entry workloads don't benefit from serialization caching**: SAC
  transfers involve small ContractData entries (~200 bytes) where XDR
  serialization is fast. Caching shines for large entries (e.g., WASM modules
  of 64KB+) but provides negligible benefit for small entries.
- **`metered_clone` of small StorageMaps is cheap**: For workloads with few
  entries per TX (like SAC transfers with ~5-8 entries), the `metered_clone`
  cost is dominated by Rc reference counting, not data copying.
- **Oracle's estimate was too optimistic**: The 6-12% estimate assumed that
  `get_ledger_changes` was a larger fraction of per-TX time than it actually
  is. The real fraction is ~2-4%, capping maximum possible improvement.
- After experiments 006-009 all showing ≤1.4% improvement from Rust-side
  micro-optimizations, the evidence strongly suggests that per-TX Rust
  execution is not the binding constraint. The ~250µs per-TX time is spread
  across many small operations with no single dominant bottleneck.
- Future optimization efforts should focus on:
  (a) The sequential `finalizeLedgerTxnChanges` phase (187ms/ledger, ~15% of
      total), especially `addLiveBatch` (106ms)
  (b) Reducing the number of parallel threads' synchronization overhead
  (c) C++ side optimizations (DB writes, bucket operations)

## Conclusion
Reverted. The optimization was correctly implemented and all tests passed, but
produced only +1.4% TPS improvement — within noise range. The fundamental issue
is that `get_ledger_changes` is only ~2-4% of per-TX time for SAC transfers,
and the cache lookup + budget charge overhead nearly equals the avoided
serialization cost for small entries. Four consecutive Rust micro-optimization
experiments (006-009) have all yielded ≤1.4%, indicating the Rust execution
path is well-optimized for this workload.
