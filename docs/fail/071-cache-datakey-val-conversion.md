# Experiment 071: Cache DataKey-to-Val Conversion in balance.rs

## Status: FAILED (-3.0% regression)

## Baseline
- TPS: ~19,520
- Profile: max-sac-tps-070b.tracy

## Result
- TPS: 18,944 [18,944, 19,072]
- Profile: max-sac-tps-071.tracy
- Change: -576 TPS (-3.0%)

## Hypothesis

Each SAC transfer performs 6 `DataKey::Balance(addr).try_into_val(e)?` conversions
across `read_balance`, `spend_balance`, `receive_balance`, and `write_contract_balance`.
The `DataKey::Balance` is a `#[contracttype]` enum, and each `try_into_val` creates
2-3 host objects (HostVec for enum discriminant+data, HostMap for Address). Since
`Val` is `Copy` (u64 wrapper), caching the converted `Val` and reusing it should
eliminate ~4 redundant conversions per transfer, saving ~8-12 host object allocations.

Additionally, `try_get_contract_data` was changed from `has_contract_data` +
`get_contract_data` (two storage lookups) to a single `try_get` (one lookup).

## Changes Made

### balance.rs
- Added `Val` to imports
- `write_contract_balance`: changed signature from `addr: Address` to `key_val: Val`
  (eliminates DataKey reconstruction and 2 conversions)
- `read_balance`: convert `DataKey::Balance(addr)` to `Val` once, reuse for both
  `try_get_contract_data` and `extend_contract_data_ttl`
- `receive_balance`: convert once, pass `key_val` to `write_contract_balance`
- `spend_balance_no_authorization_check`: same pattern
- `spend_balance`: same pattern
- `write_authorization`: same pattern

### storage_utils.rs
- `try_get_contract_data`: replaced `has_contract_data` + `get_contract_data` with
  single `try_get` lookup, extracting `ContractData` val directly

### data_helper.rs
- Two sites in `put_contract_data` and `create_contract_tombstone`: replaced
  `has` + `get_with_live_until_ledger` with single `try_get_full` lookup

## Why It Failed

1. **Val caching doesn't reduce host object creation**: `Val` is just a u64 handle
   to already-created host objects. The expensive part is the `try_into_val` conversion
   which creates HostVec/HostMap objects. Caching the resulting `Val` avoids
   re-calling `try_into_val`, but the host objects created during the *first*
   conversion are the same count — `Val` just points to them.

2. **No reduction in visit_host_object calls**: Both baseline (070b) and experiment
   (071) show exactly 7,488,000 `visit host object` calls (117 per TX). The
   optimization did NOT reduce host object traffic at all.

3. **try_get_contract_data refactor added overhead**: Replacing `has` + `get` with
   a single `try_get` required `storage_key_from_val` which does its own conversion
   work. The net effect was neutral to slightly negative.

4. **SAC transfer self-time increased**: 528ms (071) vs 499ms (070b), suggesting
   the changes to `try_get_contract_data` and the different function signatures
   introduced small but measurable overhead.

## Tracy Comparison (self-time, 30s capture)

| Zone | 070b | 071 | Delta |
|------|------|-----|-------|
| SAC transfer | 499ms | 528ms | +5.8% |
| visit host object | 514ms (7.49M calls) | 525ms (7.49M calls) | +2.1% |
| drop host extract storage | 332ms | 313ms | -5.7% |
| write xdr | 269ms (320K) | 262ms (320K) | -2.6% |

## Key Insight

To actually reduce host object allocations in SAC transfers, one would need to
avoid creating the intermediate `Val` representation entirely — e.g., by working
directly with `LedgerKey`/`Rc<LedgerKey>` for storage lookups instead of going
through the `Val → storage_key_from_val → LedgerKey` conversion chain. The
`try_into_val` caching approach attacks the wrong level of the stack.

## Reverted
All changes reverted via `git checkout -- .` in the p25 submodule.
