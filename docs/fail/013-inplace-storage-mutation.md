# Experiment 013: In-Place Storage Mutation (Eliminate Copy-on-Write)

## Date
2026-02-20

## Hypothesis
The Soroban `MeteredOrdMap::insert()` is fully copy-on-write: every `storage.put()`
and TTL extension clones the entire backing `Vec` even when just updating an
existing value. In enforcing mode (normal execution), keys are guaranteed to
already exist (checked by footprint), so we can mutate values in-place via
index-based access, eliminating O(n) allocation and copy per write.

Additionally, `storage.get()` performs two binary searches — one on the footprint
map (`enforce_access`) and one on the storage map (`self.map.get()`). Since both
maps share the same sorted keys (built from the same footprint), the index from
the footprint search can be reused to index directly into the storage map,
eliminating the second binary search.

Combined savings target: ~500ms/ledger from eliminating `new map` (402ms/6
ledgers) and reducing `map lookup` (1,328ms/6 ledgers) by ~50%.

## Change Summary

1. **`metered_map.rs` — `MeteredOrdMap`**: Added three new methods:
   - `find_index<Q>()` — public wrapper around internal `find` returning `Option<usize>`
   - `get_value_mut_at_index()` — returns `&mut V` at index (no clone)
   - `set_value_at_index()` — sets value at index in place (no reallocation)

2. **`storage.rs` — `Footprint`**: Added `enforce_access_with_index()` that
   returns the found footprint index alongside the access type, using `find_index`
   instead of `get`.

3. **`storage.rs` — `Storage::try_get_full_helper`**: In enforcing mode, uses
   one binary search on footprint + index-based storage map access (avoids
   second binary search).

4. **`storage.rs` — `Storage::put_opt_helper`**: In enforcing mode, uses
   `enforce_access_with_index` + `set_value_at_index` for in-place mutation
   (avoids copy-on-write clone of entire map).

5. **`storage.rs` — `Storage::apply_ttl_extension`**: In enforcing mode, uses
   `find_index` + `set_value_at_index` for in-place TTL update.

## Results

### TPS
- Baseline: 14,144 TPS
- Run 1: 14,400 TPS (+1.8%)
- Run 2: 14,144 TPS (+0.0%)
- Run 3: 14,144 TPS (+0.0%)
- Average: ~14,229 TPS (+0.6%)

### Tracy Analysis
Tracy capture failed (binary not found at expected path), so no per-zone
breakdown is available for this experiment. The TPS results alone are
sufficient to determine failure.

## Why It Failed
Despite targeting real copy-on-write overhead visible in the baseline Tracy
profile (`new map`: 402ms/6 ledgers = 67ms/ledger, `map lookup`: 1,328ms/6
ledgers = 221ms/ledger), the optimization produced no measurable TPS gain.

Likely explanations:
1. **The maps are small**: Each SAC transfer touches only a few storage keys
   (2-3 entries per TX for balances + authorization). With small maps (N < 10),
   the cost of cloning the Vec is trivial — just a few cache lines. The 402ms
   `new map` self-time spread across 594K calls is only ~0.7μs per call.
2. **The overhead is on worker threads, not the critical path**: The copy-on-write
   happens inside `invoke_host_function` on parallel worker threads. If workers
   are not fully saturating their time budget, saving microseconds per call
   doesn't reduce the wall-clock time of the parallel phase.
3. **Binary search on small maps is already fast**: Binary search on 2-10
   elements is essentially a few comparisons. Eliminating the second search
   saves nanoseconds.

## Files Changed
- `src/rust/soroban/p26/soroban-env-host/soroban-env-host/src/host/metered_map.rs` — Added `find_index`, `get_value_mut_at_index`, `set_value_at_index`
- `src/rust/soroban/p26/soroban-env-host/soroban-env-host/src/storage.rs` — Enforcing-mode optimizations for get, put, and TTL extension
