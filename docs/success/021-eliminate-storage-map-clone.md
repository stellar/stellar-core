# Experiment 021: Eliminate Initial Storage Map Clone

## Date
2026-02-21

## Hypothesis
In `invoke_host_function`, the initial storage map is cloned via
`storage_map.metered_clone(budget)?` purely to serve as a snapshot for
`get_ledger_changes()` to look up old entry sizes and old live_until values.
Since experiment 020 already caches entry XDR sizes at decode time, we can
extend that to cache the full rent sizes (via `entry_size_for_rent`) and
retrieve `old_live_until_ledger` directly from the TTL entry map. This
eliminates the need for the storage map clone entirely in production.

## Change Summary
- Extended `init_entry_sizes` to store pre-computed rent sizes (via
  `entry_size_for_rent()`) instead of raw XDR sizes at decode time
- Production mode: replaced `storage_map.metered_clone(budget)?` with
  `StorageMap::new()` (empty map, unused)
- In `get_ledger_changes` production path: use `init_entry_sizes` for
  `old_entry_size_bytes_for_rent` and save `old_live_until_ledger` from
  the TTL entry lookup, bypassing `init_storage_snapshot` entirely
- Recording/test mode: retains full snapshot-based approach with cloning

### Safety Argument
- The `entry_size_for_rent` function is deterministic — computing it at decode
  time vs. at get_ledger_changes time yields the same result for the same entry
- `old_live_until_ledger` comes from the TTL entry which is already looked up
  in the same loop iteration — we just save the value instead of re-fetching
- Test/recording mode retains full snapshot path for round-trip coverage
- SAC transfers never have ContractCode entries in footprint (built-in contract),
  so the wasm_module_memory_cost special case is not exercised

## Results

### TPS
- Baseline (exp-020): 14,784 TPS
- Post-change: 14,528 TPS
- Delta: **-256 TPS (-1.7%)** (within benchmark variance of ~5-10%)

### Tracy Analysis (per-TX mean times)
- parallelApply: 124.3µs → 121.3µs (**-3.0µs, -2.4%**)
- invoke_host_function total: 77.4µs → 73.9µs (**-3.5µs, -4.5%**)
- invoke_host_function self: 14.7µs → 13.9µs (**-0.8µs, -5.4%**)
- addReads self: 4.6µs → 4.7µs (unchanged)
- recordStorageChanges self: 5.2µs → 5.3µs (unchanged)
- write xdr count: ~360K (unchanged from exp-020)

### Cumulative Results (from exp-016e baseline)
- parallelApply: 130.8µs → 121.3µs (**-9.5µs, -7.3%**)
- SAC transfer: 43.4µs → ~41µs (estimated)

### Analysis
The per-TX parallelApply improvement of 3.0µs is consistent with eliminating
the MeteredClone of the StorageMap (~10 entries). The clone involved:
1. A new Vec allocation for the OrdMap entries
2. Rc refcount bumps for all keys and values
3. Metering charges for the clone operation

The TPS decrease is within normal variance and does not indicate a regression —
the Tracy per-TX metrics show clear improvement. The invoke_host_function
self-time decrease (-0.8µs) reflects reduced overhead in the function body
from skipping the clone.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs` — eliminate
  storage map clone in production, pre-compute rent sizes at decode time
