# Experiment 025: Cache Host in Thread-Local Storage

## Date
2026-02-21

## Hypothesis
The Host lifecycle (creation in `host setup` at 2.1µs + destruction in
`drop host extract storage` at 5.1µs) costs 7.2µs per TX. Caching the Host
in thread-local storage and reusing it across TXs would eliminate the
expensive Rc::try_unwrap + HostImpl drop and skip Host allocation, saving
most of the 7.2µs.

## Change Summary
- Added `try_finish_reusable(&self)` to Host: extracts storage/events via
  `std::mem::take` without consuming the Host, resets per-TX execution state
  (objects, context_stack, events, authorization_manager, etc.)
- Added `reset_storage_for_new_tx(&self, storage: Storage)` to Host
- Added `HOST_CACHE` thread-local `RefCell<Option<Host>>` in e2e_invoke.rs
- Modified host setup to reuse cached Host when available
- Modified try_finish to use `try_finish_reusable` and cache the Host back
- Gated behind `cfg(not(any(test, feature = "recording_mode")))` for
  production only

## Results

### TPS
- Baseline (exp-024): 14,656 TPS
- Post-change (exp-025): 14,784 TPS (+0.9%, within noise)
- Post-change (exp-025b, with module_cache skip fix): 14,656 TPS (no change)

### Tracy Analysis

#### exp-025 (initial implementation)
| Zone | exp-024 (ns) | exp-025 (ns) | Delta |
|------|-------------|-------------|-------|
| host setup self | 2,060 | 5,692 | +3,632 |
| drop host extract storage | 5,113 | (eliminated) | -5,113 |
| reset host for reuse | (new) | 608 | +608 |
| **Total lifecycle** | **7,173** | **6,300** | **-873** |

#### exp-025b (with diagnostic sub-zones and module_cache skip fix)
| Zone | Total (ns) | Self (ns) |
|------|-----------|-----------|
| host setup | 8,332 | 288 |
| host create or reuse | 71 | 71 |
| deser inputs | 1,764 | 535 |
| configure host | 6,207 | 5,420 |

**Key finding**: `configure host` (the setter methods) takes 6.2µs on a
cached host vs the entire host setup being 4.1µs on a fresh host. The
cached host path is SLOWER, not faster.

**parallelApply mean**: 121,346ns (exp-024) → 123,558ns (exp-025b) — WORSE

### Why It Failed

The Host caching approach suffers from an unexpected regression in the
`configure host` phase. The setter methods (`set_source_account`,
`set_ledger_info`, `set_authorization_entries`, `set_base_prng_seed`,
`set_module_cache`) collectively take ~6.2µs on a cached/reused host vs
~4.1µs when part of fresh host setup.

Possible causes:
1. **Dropping old state**: Each setter on a cached host must first drop
   the old value before writing the new one. Even though `try_finish_reusable`
   resets some fields, others (source_account, ledger_info, base_prng,
   module_cache) retain old values that must be dropped.
2. **Cache/memory effects**: The cached Host's HostImpl struct is larger in
   memory (contains allocated-but-cleared Vecs, old state) which may hurt
   CPU cache performance for the setter operations.
3. **Budget interaction**: The metered operations on a cached host may
   interact differently with the budget tracking, adding overhead.

The net effect is that the 5.1µs destruction savings is more than offset
by the ~2.2µs setup overhead increase, making the optimization a net
negative for parallelApply latency.

## Files Changed (reverted)
- `src/rust/soroban/p25/soroban-env-host/src/host.rs` — try_finish_reusable,
  reset_storage_for_new_tx
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs` — HOST_CACHE
  thread-local, cached host usage
