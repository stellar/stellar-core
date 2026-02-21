# Experiment 012: Skip encoded_key serialization in production mode

## Date
2026-02-21

## Hypothesis
The `get_ledger_changes` function serializes every LedgerKey in the storage
map to `entry_change.encoded_key` via `metered_write_xdr`. But downstream
callers (`extract_ledger_effects`, `extract_rent_changes`) never read
`encoded_key` -- only the simulation crate uses it (via its own invoke path).
Skipping this serialization should save ~2 `write_xdr` calls per invocation.

## Change Summary
Used `#[cfg(any(test, feature = "recording_mode"))]` to conditionally compile
the `encoded_key` serialization. In production (non-test, non-recording) mode,
the field is left as an empty `Vec<u8>`.

For the hash computation fallback (when no TTL entry exists), added a cfg-gated
alternative that serializes the key to a temporary buffer instead of reusing
`encoded_key`.

## Results

### TPS
- Baseline: 13,632 TPS
- Post-change: 14,272 TPS
- Delta: +4.7% / +640 TPS

### Tracy Analysis
- write_xdr calls dropped from 9.0 to 7.0 per invocation (saving 2 encoded_key serializations)
- Total write_xdr volume reduced accordingly

### Failed Variant
An extended version of this optimization also included position-based old entry
size lookup (to eliminate 2 more write_xdr calls for old entry size computation).
This caused a 20% TPS regression (10,944 TPS) despite per-invocation time
improving from 250us to 209us in Tracy. The root cause was likely increased
overhead from the extra `storage_map.iter(budget)?` call and pointer tracking
logic in `build_storage_map_from_xdr_ledger_entries`. The position-based indexing
was reverted; only the encoded_key skip is kept.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs` -- cfg-gated
  `encoded_key` serialization in `get_ledger_changes`; added temporary buffer
  hash computation fallback for production mode.

## Commit
See git log for commit hash.
