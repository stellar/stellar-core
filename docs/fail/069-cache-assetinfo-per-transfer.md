# Experiment 069: Cache AssetInfo once per SAC transfer

## Hypothesis

Each SAC transfer reads AssetInfo from Instance storage 6 times (via
`read_asset()` and `read_asset_info()`) and Metadata once (via `read_name()`).
By reading AssetInfo once at the top of `transfer()` and passing `&AssetInfo`
through the call chain, we can eliminate 5 redundant Instance storage reads
per transfer, saving ~127ms/ledger of `get_contract_data` time.

## Changes

### contract.rs
- Read `AssetInfo` once at the top of `transfer()`, `transfer_from()`,
  `burn()`, `burn_from()`, and `mint()`
- Pass `&AssetInfo` to `spend_balance`, `receive_balance`, and
  `transfer_maybe_with_issuer`

### balance.rs
- Changed `spend_balance` and `receive_balance` to accept `&AssetInfo`
- Added `_with_asset_info` variants of helper functions:
  `is_account_authorized_with_asset_info`,
  `transfer_classic_balance_with_asset_info`,
  `is_asset_auth_required_with_asset_info`,
  `is_asset_clawback_enabled_with_asset_info`,
  `is_asset_issuer_flag_set_with_asset_info`
- These functions convert `AssetInfo` -> `Asset` or match on `AssetInfo`
  directly, avoiding redundant `read_asset_info()` calls

### event.rs
- Changed `transfer_maybe_with_issuer` and `is_issuer` to accept `&AssetInfo`
- Removed `read_asset_info` import

### stellar_asset_contract.rs (test)
- Updated `test_custom_account_auth` expect! macro: instructions 828398 -> 825643,
  mem_bytes 1216862 -> 1216758

### e2e_invoke.rs
- Added `#[allow(unused_variables, unused_mut, unused_assignments)]` for
  pre-existing test-mode warning on `old_live_until_from_ttl`

## Result: FAILURE (-3.0%)

- Baseline: 19,520 TPS (experiment 068)
- After: 18,944 TPS [18,944, 19,072]
- Change: -576 TPS (-3.0%)

## Analysis

The instruction count decrease (828398 -> 825643 = -2755 instructions per mint)
confirms the optimization is logically correct -- fewer host calls are made.
However, the TPS regression suggests:

1. Instance storage reads are already effectively free at the host level (the
   Host caches instance data in memory), so eliminating them saves negligible
   time
2. The overhead of the new code path (extra `&AssetInfo` parameter threading,
   pattern matching in `_with_asset_info` functions, AssetInfo->Asset
   conversions) is more expensive than the reads it eliminates
3. The additional code complexity may inhibit compiler optimizations (larger
   functions, more branches)

## Key Learning

Instance storage reads for the same contract instance are already cached by the
Host and are essentially free. Optimizing them away at the Rust source level
adds overhead without meaningful savings. Future optimization should focus on
zones with genuine self-time costs rather than repeated reads that hit caches.

## Tracy trace

`/mnt/xvdf/tracy/max-sac-tps-069.tracy`
