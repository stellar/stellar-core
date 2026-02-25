# Experiment 076: Vec Reuse in invocation_metering.rs — FAILED

## Hypothesis

`try_snapshot_storage_and_event_resources()` in `invocation_metering.rs` creates a fresh `Vec::<u8>::new()` for each XDR serialization (lines 743, 756) just to measure serialized size, then discards the Vec. By declaring a single `Vec<u8>` before the footprint iteration loop and reusing it with `.clear()`, we eliminate per-entry heap allocation/deallocation overhead.

## Change

**File**: `src/rust/soroban/p25/soroban-env-host/src/host/invocation_metering.rs`

- Moved `let mut buf = Vec::<u8>::new();` before the `for` loop (after line 720)
- Replaced both `let mut buf = Vec::<u8>::new();` at lines 743 and 756 with `buf.clear();`

This reuses a single buffer that grows to the max entry size and stays allocated across loop iterations.

## Results

- **Tests**: All 66 passed (49,011 assertions)
- **Run 1**: 19,520 TPS [19,520, 19,648] (baseline: 19,712)
- **Run 2**: 18,944 TPS [18,944, 19,072]
- **Verdict**: No improvement. Both runs at or below baseline.

## Analysis

The optimization targeted ~660K small Vec allocations per benchmark run (3 footprint entries × 2 allocations × 109K transactions). However:

1. **Small buffers**: Each Vec holds only ~100-200 bytes of serialized LedgerEntry XDR. The system allocator handles these from thread-local free lists very efficiently — the allocation cost is ~10-20ns.
2. **Few iterations per invocation**: The SAC transfer footprint has only ~3 entries, so the loop only runs 3 times per call. The benefit of reuse is minimal compared to a scenario with many entries.
3. **Already not a top hotspot**: `metered_write_xdr` at 445ms total includes the actual serialization work, not just allocation. The allocation portion is a small fraction.

## Conclusion

Vec allocation for small, short-lived buffers in a tight loop with only 3 iterations is not a meaningful bottleneck. The allocator's fast path handles these efficiently. Reverted.
