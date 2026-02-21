# Experiment 020: Cache Initial Entry XDR Sizes

## Date
2026-02-21

## Hypothesis
In `get_ledger_changes()`, old ledger entries are XDR-serialized purely to
compute their byte size for rent calculation, then the buffer is discarded.
Since XDR is a deterministic encoding, the original input buffer length (known
at decode time in `build_storage_map_from_xdr_ledger_entries`) equals the
re-encoded length. By caching these sizes at decode time and passing them to
`get_ledger_changes`, we can skip the re-serialization entirely.

## Change Summary
- Modified `build_storage_map_from_xdr_ledger_entries()` to record each entry's
  original XDR size (`entry_buf.as_ref().len()`) alongside its key in a
  `Vec<(Rc<LedgerKey>, u32)>`
- Added `init_entry_sizes` parameter to `get_ledger_changes()` (production only,
  gated with `#[cfg(not(any(test, feature = "recording_mode")))]`)
- In production, old entry size is looked up from the cached sizes instead of
  re-serializing via `metered_write_xdr`
- Test/recording mode retains the full serialization path for XDR round-trip
  coverage

### Safety Argument
- XDR is a deterministic canonical encoding: decode(buf).encode() == buf
- The cached sizes are recorded at the exact point of decoding, ensuring
  consistency
- Test builds retain full serialization, exercising the round-trip path

## Results

### TPS
- Baseline (exp-019): 14,144 TPS
- Post-change: 14,784 TPS
- Delta: **+640 TPS (+4.5%)**

### Tracy Analysis (per-TX mean times)
- parallelApply: 124.9us -> 124.3us (**-0.6us, -0.5%**)
- write xdr count: 442,629 -> 359,613 (**-83,016 calls, -18.8%**)
- write xdr total time: 404.5ms -> 288.0ms (**-116.5ms, -28.8%**)
- Per-TX XDR savings: ~1.62us per TX

### Cumulative Results (from exp-016e baseline)
- parallelApply: 130.8us -> 124.3us (**-6.5us, -5.0%**)
- SAC transfer: 43.4us -> 41.9us (**-1.5us, -3.5%**)

### Analysis
The write xdr count dropped by 83,016 calls (~1.15 per TX), confirming that
old entry serializations are being skipped. The total write xdr time decreased
by 28.8% (116.5ms over the 30s sample).

The per-TX parallelApply improvement is modest (-0.6us) because:
1. The lookup in init_entry_sizes uses linear scan with LedgerKey comparison,
   which adds some overhead (~50-100ns per lookup)
2. Budget metering charges during serialization were already cheap (cost models
   return 0 with disabled metering)
3. The main savings come from avoided Vec allocations and XDR write traversals

The TPS improvement (+4.5%) exceeds the per-TX savings because the reduced
serialization also benefits the sequential per-ledger overhead path.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs` — cache initial
  entry XDR sizes, skip old-entry re-serialization in production
