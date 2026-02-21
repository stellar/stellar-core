# Experiment 026: Tracy Diagnostic Zones + xdr_size Deferral

## Date
2026-02-21

## Hypothesis
Adding detailed Tracy zones to addReads, recordStorageChanges, and
invokeHostFunction will reveal sub-function bottlenecks for targeted
optimization. Also, deferring xdr_size computation for Soroban entries
(which don't need disk metering on p23+) will save a small amount per TX.

## Change Summary
- **addReads xdr_size deferral**: Skip `xdr::xdr_size(lk)` for Soroban
  entries on p23+ (they're in-memory, no disk read metering needed). Only
  compute when entering the disk metering branch.
- **recordStorageChanges xdr_size deferral**: Move `xdr::xdr_size(lk)`
  inside the `lk.type() != TTL` branch since TTL entries don't need it.
- **invokeHostFunction refactor**: Pre-serialize all inputs (hostFunction,
  resources, sourceID, auth entries, basePrngSeed) in a single zone before
  the Rust bridge call.
- **Tracy zones added**:
  - `addReads: getLedgerEntryOpt TTL` — TTL entry loading
  - `addReads: getLedgerEntryOpt` — actual entry loading
  - `addReads: toCxxBuf entry` — entry XDR serialization
  - `addReads: toCxxBuf TTL` — TTL XDR serialization
  - `invokeHostFunction: serialize inputs` — all C++ input serialization
  - `recordStorageChanges: xdr_from_opaque` — entry deserialization
  - `recordStorageChanges: upsertLedgerEntry` — entry write-back

## Results

### TPS
- Baseline (exp-024): 14,656 TPS
- Post-change (exp-026): 14,784 TPS (+0.9%, within noise)

### Tracy Analysis — addReads Breakdown (per TX, 2 calls)

| Zone | Total (ns) | Self (ns) |
|------|-----------|----------|
| addReads total | 16,418 | 2,970 |
| getLedgerEntryOpt TTL | 3,362 | 2,782 |
| getLedgerEntryOpt entry | 4,288 | 3,078 |
| toCxxBuf entry | 1,132 | 1,132 |
| toCxxBuf TTL | 100 | 100 |

**Key finding**: toCxxBuf (XDR serialization) is cheap — only 1.2µs per TX.
The real cost is getLedgerEntryOpt (loading entries from in-memory state)
at 7.6µs total.

### Tracy Analysis — recordStorageChanges Breakdown (per TX)

| Zone | Total (ns) | Self (ns) |
|------|-----------|----------|
| recordStorageChanges total | 13,378 | 3,192 |
| xdr_from_opaque (×3) | 1,668 | 1,668 |
| upsertLedgerEntry (×3) | 7,746 | 795 |
| → upsertEntry (×3) | 6,951 | 5,598 |

**Key finding**: xdr_from_opaque is cheap (556ns/entry). upsertEntry
dominates at 1,866ns self-time per call (5.6µs per TX for 3 entries).

### Tracy Analysis — invokeHostFunction (per TX)

| Zone | Total (ns) | Self (ns) |
|------|-----------|----------|
| invokeHostFunction total | 87,895 | 2,882 |
| serialize inputs | 1,510 | 1,510 |

**Key finding**: Input serialization is only 1.5µs. Most of the C++
wrapper overhead (2.9µs self) is try/catch, error handling, metrics.

### Complete Per-TX Self-Time Budget (top items within apply path)

| Zone | Per TX Self (ns) | % of 122µs |
|------|-----------------|-----------|
| SAC transfer | 8,092 | 6.6% |
| visit host object (×117) | 8,190 | 6.7% |
| upsertEntry (×3) | 5,598 | 4.6% |
| e2e_invoke self | 4,168 | 3.4% |
| invoke function/return val | 4,385 | 3.6% |
| obj_cmp (×16) | 4,064 | 3.3% |
| write xdr (×5) | 3,965 | 3.3% |
| read xdr with budget (×4) | 3,808 | 3.1% |
| map lookup (×44) | 3,828 | 3.1% |
| recordStorageChanges self | 3,192 | 2.6% |
| getLedgerEntryOpt entry | 3,078 | 2.5% |
| addReads self | 2,970 | 2.4% |
| Val to ScVal (×24) | 2,904 | 2.4% |
| invokeHostFunction self | 2,882 | 2.4% |
| getLedgerEntryOpt TTL | 2,782 | 2.3% |
| ... remaining zones | ~56,540 | 46.3% |

**Note**: Rust binary was stale (had exp-025b host caching code compiled
in). host setup showed 8.2µs (due to configure host overhead) vs expected
4.1µs. try_finish showed 2.9µs (due to non-destructive finish) vs
expected 7.3µs. Net effect is neutral (~0.3µs difference). C++ zone data
is accurate regardless.

## Optimization Opportunities Identified

1. **upsertEntry getLiveEntryOpt check** (5.6µs): For entries known to
   exist from the readWrite footprint, skip the existence check.
2. **getLedgerEntryOpt chain** (7.6µs): Two-level map lookup (TX map +
   thread map) for each entry. Could be optimized with pre-population.
3. **recordStorageChanges self** (3.2µs): LedgerEntryKey creation + hash
   set operations for each modified entry.
4. **visit host object** (8.2µs): 117 calls per TX at 70ns each. Budget
   charge still included despite targeted optimizations.

## Files Changed
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — xdr_size deferral
  in addReads and recordStorageChanges, Tracy zones, input serialization
  refactor
