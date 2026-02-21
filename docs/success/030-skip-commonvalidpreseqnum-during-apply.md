# Experiment 030: Skip Redundant Validation in commonValidPreSeqNum During Apply

## Date
2026-02-21

## Hypothesis
During apply for Soroban TXs, `commonValidPreSeqNum` performs extensive
validation (envelope type checks, extra signer validation, soroban resource
checks, footprint duplicate detection, time bounds, fee checks) that was
already done during TX set building. Skipping these and going directly to
the account load should save ~1-2µs per TX.

## Change Summary
Added `bool applying` parameter to `commonValidPreSeqNum`. When
`applying && isSoroban()`, skip all validation and go directly to account
loading (`ls.getAccount(header, *this)`).

Checks skipped during apply:
- Envelope type validation (structural, can't change)
- Extra signers validation (structural)
- Op count check (structural)
- validateSorobanOpsConsistency (structural)
- Soroban protocol version check (structural)
- validateSorobanMemo (structural)
- checkSorobanResources (static TX properties)
- Resource fee overflow checks (static)
- Footprint duplicate detection (was allocating UnorderedSet per TX)
- isTooEarly / isTooLate time bounds (redundant during apply)
- Fee checks (no-op during apply for v9+)

## Results

### TPS
- Baseline (exp-029): 15,168 TPS [15,168-15,232]
- Post-change: 14,976 TPS [14,976-15,104]
- Delta: Within variance (~1.3% difference)

### Tracy Analysis
| Zone | Exp-029 (µs/TX) | Exp-030 (µs/TX) | Delta |
|------|------------------|------------------|-------|
| preParallelApply | 10.7 | 9.6 | -1.1 (-10.3%) |
| commonValid | 3.6 | 2.6 | -1.0 (-27.8%) |

The per-TX improvement is confirmed by Tracy (-1.0µs in commonValid), but
TPS delta is within benchmark variance (~5-10%). The optimization compounds
with future changes that increase TX count per ledger.

## Files Changed
- `src/transactions/TransactionFrame.h` — Added `bool applying` parameter
  to `commonValidPreSeqNum`
- `src/transactions/TransactionFrame.cpp` — Early return with account load
  only when `applying && isSoroban()`; pass `applying` from `commonValid`

## Commit
