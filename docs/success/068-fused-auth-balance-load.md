# Experiment 068: Fused Auth-Check + Balance-Load in SAC Transfer

## Date
2026-02-24

## Hypothesis
In the SAC transfer path for Contract addresses, `is_authorized` reads the
balance entry from persistent storage, then `spend_balance` / `receive_balance`
reads the same balance entry again. For receivers (new contracts with no
existing balance), `receive_balance` calls `try_get_contract_data` in
`is_authorized`, then calls it again inside `receive_balance_check`. Each
redundant Persistent storage read goes through MeteredOrdMap binary search,
XDR deserialization, and metering overhead.

By fusing the auth-check and balance-load into a single operation for Contract
addresses, we eliminate one duplicate Persistent storage read per sender and
one per receiver, saving ~128K storage reads per ledger (64K transfers x 2).

## Change Summary
Modified `spend_balance` and `receive_balance` in `balance.rs` to handle
Contract addresses with a fused code path:

- **`receive_balance`** (lines 92-163): For Contract addresses, performs a
  single `try_get_contract_data` call. If the balance exists, checks the
  `authorized` flag inline (replicating the `is_authorized` logic). If the
  balance doesn't exist, checks `is_asset_auth_required` to determine if
  unauthorized balances should be rejected. Avoids the separate
  `is_authorized` + `receive_balance_check` + second `try_get_contract_data`
  pattern.

- **`spend_balance`** (lines 230-305): For Contract addresses, reads the
  balance entry once via `get_contract_data`, checks the `authorized` flag
  inline, then performs the balance deduction. Avoids the separate
  `is_authorized` (which reads balance) + `spend_balance_no_authorization_check`
  (which reads balance again) pattern.

- Account addresses still delegate to the original `is_account_authorized` +
  `transfer_classic_balance` path (unchanged).

- All error semantics preserved: deauthorized errors are raised before
  insufficient-balance errors, matching the original behavior.

## Results

### TPS
- Baseline: 19,264 TPS (interval [301, 302])
- Post-change: 19,520 TPS (interval [305, 307])
- Delta: +256 TPS (+1.3%)

### Analysis
The improvement comes from eliminating ~128K redundant Persistent storage
reads per ledger. Each read involves MeteredOrdMap binary search over the
storage map (~6-8 entries per contract), XDR field access, and metering
charges. The 107 fewer metered instructions per transfer (828505 -> 828398)
confirmed by the `test_custom_account_auth` expect! macro update validates
that the optimization reduces real work.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/builtin_contracts/stellar_asset_contract/balance.rs`
  — Fused auth-check + balance-load in `spend_balance` and `receive_balance`
  for Contract addresses.
- `src/rust/soroban/p25/soroban-env-host/src/test/stellar_asset_contract.rs`
  — Updated `expect!` macro: `instructions: 828505` -> `instructions: 828398`.
- `src/rust/soroban/p25/soroban-env-host/observations/25/*.json` — 35
  observation files updated via `UPDATE_OBSERVATIONS=1` to reflect reduced
  instruction counts and changed storage access patterns.

## Commit
<to be filled after commit>
