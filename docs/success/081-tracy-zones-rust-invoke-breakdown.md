# Experiment 081: Add Tracy Zones to Unzoned Rust Functions

## Date
2026-02-25

## Hypothesis
The `e2e_invoke::invoke_function` zone had 663ms of self-time (exp 080) with no
child zones for several functions called after host function execution:
`extract_rent_changes`, `host_compute_rent_fee`, `extract_ledger_effects`,
`encode_diagnostic_events`, and budget metric extraction. Adding Tracy zones to
these functions will reveal where the time is spent and guide future
optimizations.

## Change Summary
Added `tracy_span!()` zones to 5 unzoned call sites in
`src/rust/src/soroban_proto_any.rs` within `invoke_host_function_or_maybe_panic`:

1. **`budget metric extraction`** — wraps `get_cpu_insns_consumed`, `get_mem_bytes_consumed`, `get_tracker`, and `get_time` calls
2. **`extract_rent_changes`** — wraps the call to `extract_rent_changes()`
3. **`host_compute_rent_fee`** — wraps the call to `host_compute_rent_fee()`
4. **`extract_ledger_effects`** — wraps the call to `extract_ledger_effects()`
5. **`encode_diagnostic_events`** — wraps the call to `encode_diagnostic_events()`

## Results

### TPS
- Baseline: 19,840 TPS
- Post-change: 19,712 TPS [19,712, 19,776]
- Delta: -0.6% (within noise — Tracy zones are measurement-only)

### Tracy Analysis — New Zone Breakdown (60s capture, 114,883 calls)

| Zone | Self-time (ms) | Per-call (μs) |
|------|-------|-------|
| `extract_ledger_effects` | 55.8 | 0.49 |
| `extract_rent_changes` | 9.7 | 0.08 |
| `host_compute_rent_fee` | 5.8 | 0.05 |
| `encode_diagnostic_events` | 2.3 | 0.02 |
| `budget metric extraction` | 1.4 | 0.01 |
| **Total newly accounted** | **75.0** | **0.65** |

### Key Finding
These 5 functions account for only **75ms** of the original ~663ms self-time
attributed to `e2e_invoke::invoke_function`. The remaining ~520ms is distributed
across:
- Pattern matching / `encoded_invoke_result` creation
- `RustBuf::from` / `.into()` conversions
- Contract events iterator chain
- `Instant::now()` calls and timing overhead

The `extract_ledger_effects` function is the most expensive of the newly zoned
functions at 55.8ms, but it's still relatively small compared to the top
hotspots.

### Updated Top Self-Time Zones in Apply Path (60s capture)

| Zone | Self-time (ms) | Calls | Per-call (μs) |
|------|-------|-------|-------|
| SAC transfer | 1,282 | 114,881 | 11.2 |
| upsertEntry | 722 | 229,768 | 3.14 |
| drop host extract storage | 575 | 114,883 | 5.00 |
| e2e_invoke::invoke_function | 521 | 114,881 | 4.54 |
| Host::invoke_function | 521 | 114,882 | 4.53 |
| write xdr | 464 | 574,413 | 0.81 |
| addReads | 447 | 229,760 | 1.94 |
| read xdr with budget | 425 | 459,529 | 0.93 |
| Val to ScVal | 350 | 2,527,392 | 0.14 |
| processResultAndMeta | 258 | 125,633 | 2.05 |
| invoke_host_function | 257 | 114,882 | 2.24 |
| commitChangesToLedgerTxn | 236 | 8 | 29,494 |
| ScVal to Val | 229 | 2,067,859 | 0.11 |
| build storage map | 211 | 114,882 | 1.84 |
| invokeHostFunction: serialize inputs | 211 | 114,881 | 1.83 |
| host setup | 209 | 114,882 | 1.82 |
| recordStorageChanges: xdr_from_opaque | 185 | 344,650 | 0.54 |
| invoke_host_function_or_maybe_panic | 178 | 114,881 | 1.55 |
| InvokeHostFunctionOpFrame doApply | 176 | 114,880 | 1.53 |
| build footprint | 172 | 114,882 | 1.50 |
| get_ledger_changes | 146 | 114,883 | 1.27 |

## Files Changed
- `src/rust/src/soroban_proto_any.rs` — Added 5 tracy_span! zones around
  previously unzoned function calls in invoke_host_function_or_maybe_panic

## Commit
<pending>
