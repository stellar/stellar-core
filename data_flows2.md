# Soroban parallel-apply data flows — post-optimization analysis

Companion to [data_flows.md](data_flows.md). Re-runs the data-flow walk after the 13-commit optimization pass (commits `2882f78dc..71d0a1379` on `rs_apply`). Section 1 here only highlights paths that changed substantively; for the un-changed sub-sections refer to data_flows.md. Section 2 is the updated remaining-inefficiency list: resolved items are dropped, partial-fix items stay with the remaining residue, and any new inefficiencies introduced by the optimization pass are flagged.

The shorthand for data-moving steps is unchanged from data_flows.md: **clone**, **enc**, **dec**, **sha**, **memcpy**.

---

## 1. Data flows — changes since data_flows.md

### 1.1 TxSetFrame envelope path (unchanged shape; ownership now flows through)

The C++ side still parallel-encodes envelopes to `CxxBuf` and Rust still parallel-decodes them into `Vec<TransactionEnvelope>`. The bridge round-trip is unchanged (see data_flows.md §1.1). What changed:

- `apply_soroban_phase` ([orchestrator.rs:67–91](src/rust/src/soroban_apply/orchestrator.rs#L67-L91)) now partitions the flat `envelopes: Vec<TransactionEnvelope>` into a `Vec<Vec<TransactionEnvelope>>` keyed by cluster, then `std::mem::take`s the per-cluster Vec into each worker's `move` closure ([orchestrator.rs:137–162](src/rust/src/soroban_apply/orchestrator.rs#L137-L162)). Each worker receives an owned `Vec<TransactionEnvelope>` and consumes it via `cluster.into_iter()`.
- `dispatch_one_tx` ([orchestrator.rs:633–803](src/rust/src/soroban_apply/orchestrator.rs#L633-L803)) takes `TransactionEnvelope` by value, runs the memo check on a borrow, then `extract_tx_parts_owned` ([common.rs:521–550](src/rust/src/soroban_apply/common.rs#L521-L550)) destructures into owned `(MuxedAccount, Vec<Operation>, SorobanTransactionData)`.
- The `InvokeHostFunction` arm destructures `op.body` to peel `host_function`, `auth` out of `InvokeHostFunctionOp`, and `archived_soroban_entries` out of `SorobanTransactionDataExt::V1`. All three flow into `apply_invoke_host_function` by-move.

Net result on the apply hot path: `op.host_function.clone()`, `op.auth.to_vec()`, `muxed_to_account_id(&source)` clones are gone. `resources` is still borrowed because the post-host delete-detection loop reads `resources.footprint`.

### 1.2 Classic / archived prefetch (new bridge shape)

[bridge.rs:48–58](src/rust/src/bridge.rs#L48-L58) introduces a separate struct `LedgerEntryInput { key_xdr: CxxBuf, value_xdr: CxxBuf }` for C++→Rust prefetch. The C++ encoder ([LedgerManagerImpl.cpp:1506–1517](src/ledger/LedgerManagerImpl.cpp#L1506-L1517)) now does:

```cpp
u.key_xdr.data = std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(key));
u.value_xdr.data = std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(entry));
```

— a `unique_ptr` move into the bridge struct. The per-byte `push_back` loop into `rust::Vec<u8>` is gone.

`LedgerEntryUpdate` (the Rust→C++ output shape) keeps `RustBuf` fields because Rust owns those bytes and the rust-allocated Vec is what C++ already has APIs to consume.

### 1.3 Phase-end commit (one encode + one SHA-256 per Soroban write)

`apply_phase_writes_to_state` ([orchestrator.rs:252–505](src/rust/src/soroban_apply/orchestrator.rs#L252-L505)) data/code passes now encode each `LedgerKey` exactly once locally, SHA-256 those bytes to derive the `TtlKeyHash`, and thread BOTH the encoded bytes (via `push_update_with_encoded_key`) and the hash (via `upsert_contract_{data,code}_with_key_hash` / `contains_*_by_hash` / `delete_*_by_hash`) into `SorobanState`. The `SorobanState` hash-keyed CRUD entry points ([state.rs:421–697](src/rust/src/soroban_apply/state.rs#L421-L697)) skip the redundant `ledger_entry_key + XDR encode + SHA-256` that the previous `upsert_contract_data_with_size` did internally — those wrapper methods have been removed.

### 1.4 Hashing across the Rust apply layer

Every `HashMap<LedgerKey, ...>` and `HashMap<TtlKeyHash, ...>` in the apply path is now `FastMap = rustc_hash::FxHashMap`. Same for the matching `FastSet` ([common.rs:16–22](src/rust/src/soroban_apply/common.rs#L16-L22)). Affects `AccumulatedWrites`, the classic/archived prefetch maps, `host_bytes`, `ro_ttl_bumps`, `state_entry_rc_cache`, `SorobanState`'s internal maps, `pending_ttls`, and the post-host `rw_ttl_keys` / `returned_rw_keys` / `auto_restored_keys_set` sets.

`state_entry_rc_cache` itself is now keyed by `TtlKeyHash` (`[u8;32]`) instead of `LedgerKey` ([invoke.rs:809–818](src/rust/src/soroban_apply/invoke.rs#L809-L818)). The hash was already computed earlier in the same iteration for the TTL probe; the cache reuses it. The per-insert deep `LedgerKey::clone()` (which cloned the inner SCVal for ContractData / Hash for ContractCode) is gone.

### 1.5 Auto-restore Rc sharing

The auto-restore bookkeeping vecs are now `Vec<(LedgerKey, Rc<LedgerEntry>)>` ([invoke.rs:388–399](src/rust/src/soroban_apply/invoke.rs#L388-L399)) and share the same `Rc` with the host-input `typed_ledger_entries` Vec via `Rc::clone`. The entry is allocated once per restored slot instead of three times.

### 1.6 RO TTL bumps carry encoded bytes

`ro_ttl_bumps` is `FastMap<LedgerKey, (LedgerEntry, Vec<u8>)>` ([orchestrator.rs:553–562](src/rust/src/soroban_apply/orchestrator.rs#L553-L562)). When invoke.rs sees the host return an RO TTL bump it stores the host-supplied encoded bytes alongside the typed entry ([invoke.rs:1063–1086](src/rust/src/soroban_apply/invoke.rs#L1063-L1086)). At the cluster-end drain ([orchestrator.rs:595–609](src/rust/src/soroban_apply/orchestrator.rs#L595-L609)) and the pre-RW-flush ([orchestrator.rs:711–722](src/rust/src/soroban_apply/orchestrator.rs#L711-L722)) the winning bytes fold into `host_bytes` so the phase-end commit reuses them verbatim. ExtendFootprintTtl supplies an empty byte vec; the phase-end commit re-encodes only for those slots.

### 1.7 Hot/live restore — one decode on the C++ side

`applySorobanPhaseRust` ([LedgerManagerImpl.cpp:3612–3697](src/ledger/LedgerManagerImpl.cpp#L3612-L3697)) decodes each `(key, value)` pair from `result.hot_archive_restores` / `live_restores` ONCE, stashes the per-TX result in a `PerTxDecodedRestores` struct, and emits the `markRestoredFrom*` side effect from that decoded view. `applyParallelPhase` passes the matching `PerTxDecodedRestores` element to `processSorobanPerTxResult`, which moves the maps in instead of re-decoding the same bytes.

### 1.8 Other small things

- §2.8 dropped the duplicate cost-params encode at the bridge boundary — the orchestrator reads `ledger_info.{cpu,mem}_cost_params.as_ref()` for the phase-end commit ([orchestrator.rs:212–220](src/rust/src/soroban_apply/orchestrator.rs#L212-L220)).
- §2.6 cap-check reads `host_bytes.get(k).map(|b| b.len())` for cluster-local shadows ([invoke.rs:752–760](src/rust/src/soroban_apply/invoke.rs#L752-L760)) before falling back to encode.
- §3.4 `state_entry_rc_cache` covers `CONTRACT_DATA` in addition to `CONTRACT_CODE`.

---

## 2. Remaining inefficiencies

Items below are what's left after the optimization pass. Each entry tags whether it's a holdover from data_flows.md §2 (with the original number), a partial fix (residue still present), or **NEW** (introduced by this pass).

### 2.1 LedgerKey rebuilt twice per RW write inside the host (was data_flows.md §2.8)

Reduced from three times to two. The host's [`build_storage_footprint_from_xdr`](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L1169) still `metered_clone`s every footprint key into an `Rc<LedgerKey>`, and [`build_storage_map_from_typed_ledger_entries`](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L562-L640) still rebuilds the key from the LedgerEntry via `ledger_entry_to_ledger_key` and wraps it in a fresh `Rc::metered_new`. The post-host driver no longer re-derives a third LedgerKey (the Rust driver now reads `ledger_entry_key(&owned)` once and reuses the same value for `host_bytes` + `cluster_local_writes`).

Eliminating the second materialization requires the host's typed entry point to accept pre-paired `(Rc<LedgerKey>, Rc<LedgerEntry>, ...)` tuples from the embedder (so the apply driver wraps each footprint key in an Rc once and shares it). Substantial API change; deferred.

### 2.2 `ledger_entry_to_ledger_key` deep-clones inner fields (data_flows.md §2.9, host)

Unchanged. For every typed-path RW write the host clones `account_id` / `asset` / `contract` / `key` (SCVal) / `hash` to derive the key from the entry. The clone of `key` (an SCVal for ContractData) is the heaviest. Requires the §2.1 fix above (embedder pairs keys+entries) to eliminate.

### 2.3 `storage_map.metered_clone(budget)?` per TX (data_flows.md §2.10, host)

Unchanged. The host clones the entire `StorageMap` (`MeteredOrdMap` = `Vec<(Rc<LedgerKey>, Option<EntryWithLiveUntil>)>`) to keep an immutable init snapshot for the post-host diff ([e2e_invoke.rs:485](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L485)). For a footprint of N entries the clone is O(N) Vec memcpy + N `Rc::clone` refcount bumps.

### 2.4 `Rc::try_unwrap` may still fall back to clone (data_flows.md §2.11, host)

Status uncertain. The original analysis claimed `init_storage_map` keeps `typed_new_value` Rcs at refcount ≥ 2; on re-examination init_storage_map holds the *old* (pre-host) entries while `typed_new_value` clones the *new* (post-host) entries — different Rc instances. The post-host `storage` local IS dropped before `extract_ledger_effects_typed` runs at the caller, so `try_unwrap` should already succeed. Needs an empirical `Rc::strong_count` probe before changing the host's diff algorithm.

### 2.5 Hot/cross-stage `xdr_serialized_size` re-encode for cap check (residue of data_flows.md §2.27)

Cluster-local shadow now reads `host_bytes.get(k).map(|b| b.len())`. Cross-stage shadow still calls `xdr_serialized_size(&entry_cow)` ([invoke.rs:754–755](src/rust/src/soroban_apply/invoke.rs#L754-L755)) because the per-cluster `host_bytes` doesn't contain bytes for entries written in a prior stage. Threading `accumulated_host_bytes` (phase-level) down into `apply_invoke_host_function` would close this; minor win, not worth invasive plumbing for SAC/Soroswap (they don't hit cross-stage shadows often).

### 2.6 tx_change prev re-encode (data_flows.md §2.14)

Unchanged. When `enable_tx_meta` is on, `build_tx_delta_with_cached_new` ([common.rs:472–504](src/rust/src/soroban_apply/common.rs#L472-L504)) re-encodes the prev entry via `prev.to_xdr()` for every modified entry. The prev value sits in `SorobanState` (typed) or in the writes layers (typed); there's no host-supplied bytes form for the prev side.

### 2.7 `ttl_key_hash_for` SHA-256 still hot on the read path (residue of data_flows.md §2.16)

Reduced. The phase-end commit now precomputes the hash once per write and threads it into `SorobanState`. The hot READ path in the invoke driver also reuses one hash per footprint slot across the TTL probe and the `state_entry_rc_cache` lookup. What still recomputes:

- Every `state.get(&key)` / `state.has_ttl(&key)` / `state.contains_*_by_hash`-less call path computes `ttl_key_hash_for(&key)` internally. Within a single TX iteration over the footprint the driver computes one hash; outside the driver (FFI lookups, BucketTestUtils replay) the hash is per-call.
- `ttl_lookup_key_for(k)` (computes hash and wraps in `LedgerKey::Ttl`) is called in the auto-restore branch ([invoke.rs:524, 563, 568](src/rust/src/soroban_apply/invoke.rs#L524)) and in the post-host tombstone walk ([invoke.rs:1020, 1145](src/rust/src/soroban_apply/invoke.rs#L1020)). These are separate code paths from the main `key_hash_for_soroban` computation — within one TX the same key can be hashed twice.

Hash caching on a wrapper struct (or on the typed `LedgerKey` itself) would eliminate the remaining recomputation; the current state passes precomputed hashes through narrow public APIs only.

### 2.8 Parallel-decode of Soroban writes on the C++ side (data_flows.md §2.26)

Unchanged. `applySorobanPhaseRust` runs `parallelDecodeEntries(result.soroban_init_updates)` and `parallelDecodeEntries(result.soroban_live_updates)` ([LedgerManagerImpl.cpp:3479–3559](src/ledger/LedgerManagerImpl.cpp#L3479-L3559)) before `ltx.createWithoutLoading` / `updateWithoutLoading`. The decode is parallel; the work itself remains. Typed-bridge avoidance would skip it entirely (Rust hands the typed entries straight in).

### 2.9 Diagnostic events encoded individually (data_flows.md §2.23)

Status: gated by `enable_diagnostics` (the 19 core_metrics events are not emitted on Rust side when diagnostics off, and `processSorobanPerTxResult` decodes a 0-length list). Wire format isn't the blocker for byte-through optimization; the real blocker is that `DiagnosticEventManager::finalize` returns `xdr::xvector<DiagnosticEvent>` and the meta builder reads typed events. Storing encoded bytes would require either custom xdrpp splicing (invasive) or moving the decode from the apply path to the meta-finalize path (no wall-clock win since meta runs right after apply). Defer.

### 2.10 Contract events re-decoded on the C++ side (data_flows.md §2.24)

Status: gated by `metaEnabled` (the decode loop is in `if (metaEnabled)` branch). Same constraint as §2.9: `OpEventManager::setEvents(xdr::xvector<ContractEvent>&& events)` consumes typed events for the meta path, so the bridge bytes have to be decoded somewhere before meta XDR output. The byte-through optimization requires deeper EventManager + meta-builder plumbing changes, not a wire-format change. Defer.

### 2.11 Per-TX envelope encode/decode round-trip (data_flows.md §2.1)

Unchanged. C++ `xdr_to_opaque`s every envelope (parallel) and Rust `from_xdr`s every envelope (parallel). The actual fix is a typed-input bridge for envelopes; no smaller win available without it. SKIPPED in the optimization pass.

### 2.12 ExtendFootprintTtl / RestoreFootprint paths (data_flows.md §2.13, §2.20–§2.22)

Unchanged. These op drivers were explicitly out of scope (no benchmark coverage); the same encode-then-encode-again patterns described in the original report are still present.

### 2.13 LedgerKey clones in cluster-local bookkeeping sets (residue of data_flows.md §2.32 scope)

The §2.32 fix applied to `state_entry_rc_cache` (now `FastMap<TtlKeyHash, Rc<LedgerEntry>>`). Other apply-layer sets still key by `LedgerKey`:

- `returned_rw_keys: FastSet<LedgerKey>` at [invoke.rs:1026](src/rust/src/soroban_apply/invoke.rs#L1026): `returned_rw_keys.insert(key.clone())`. Used post-host to detect deletions.
- `auto_restored_keys_set: FastSet<LedgerKey>` at [invoke.rs:1105–1110](src/rust/src/soroban_apply/invoke.rs#L1105-L1110): built from `auto_restored_data_writes` + `auto_restored_live_data` via `k.clone()`.
- `rw_ttl_keys: FastSet<LedgerKey>` at [invoke.rs:1014–1022](src/rust/src/soroban_apply/invoke.rs#L1014-L1022): each insert is `ttl_lookup_key_for(k)` which computes one SHA-256 and wraps in `LedgerKey::Ttl(...)`. Could be `FastSet<TtlKeyHash>` keyed on the 32-byte hash directly.
- `host_bytes.insert(key.clone(), ...)` at [invoke.rs:1095](src/rust/src/soroban_apply/invoke.rs#L1095): same key is also `insert`ed into `cluster_local_writes` below at [:1096](src/rust/src/soroban_apply/invoke.rs#L1096), so one of the two inserts has to clone.

These are all per-RW-write clones of the typed `LedgerKey` (inner SCVal/Hash). For workloads with many RW writes the cumulative cost is non-trivial. Rekeying by `TtlKeyHash` for the Soroban-key variants would compress these to a 32-byte memcpy — same pattern as §2.32. Not done in this pass because it requires the call sites' lookup queries to also go through `TtlKeyHash`, which means propagating the precomputed hash further (touching the post-host loop and a couple of `layered_get` callers).

### 2.14 ~~Soroban init/live `key_xdr` is dead weight on the C++ side~~ — RESOLVED (commit `c7f08595b`)

`SorobanPhaseResult` was reshaped:
- `soroban_init_entry_xdrs: Vec<RustBuf>` (was `Vec<LedgerEntryUpdate>`)
- `soroban_live_entry_xdrs: Vec<RustBuf>`
- `soroban_dead_key_xdrs: Vec<RustBuf>` (was LedgerEntryUpdate with empty value)
- `classic_updates: Vec<LedgerEntryUpdate>` unchanged

The Rust orchestrator's data/code commit loops already encoded each LedgerKey to derive the `TtlKeyHash`; on the delete path those bytes flow into `soroban_dead_key_xdrs`, on the init/live path they're dropped on the floor.

### 2.15 Per-write `staged_update: Vec::with_capacity(1)` allocation **[NEW]**

`apply_phase_writes_to_state` ([orchestrator.rs:363–379, 420–435](src/rust/src/soroban_apply/orchestrator.rs#L363-L379)) materializes a single-element Vec per data/code write just to call `push_update_with_encoded_key` then `.extend(staged_update)` into the chosen target Vec. The pattern exists because the `is_new` return from upsert decides which target Vec (init vs live) to push into, and that decision can only be made after we've consumed the entry (move-semantics force the order).

For ~thousands of Soroban writes per phase this is a small per-write `Vec` allocation. Restructure: pass both `init_updates` and `live_updates` into a helper, or split the decision earlier (compute is_new from `state.contains_*_by_hash(key_hash)` BEFORE the upsert, branch the target before calling `push_update_with_encoded_key`). The latter does an extra hash-map lookup per write but avoids the per-write allocation.

### 2.16 Per-phase `Vec<Vec<TransactionEnvelope>>` partition step **[NEW]**

`apply_soroban_phase` ([orchestrator.rs:67–91](src/rust/src/soroban_apply/orchestrator.rs#L67-L91)) iterates `envelopes.into_iter()` and partitions into per-cluster Vecs upfront, so each worker can `move` its chunk. The partition walks each envelope once and pushes it into the matching cluster Vec. Per-envelope cost is just an O(1) move (the inner allocations stay put), but it's an extra one-time per-phase walk over all 6000 envelopes. Could be eliminated by tracking the per-cluster offsets and using `Vec::drain(begin..end)` segments instead of a fresh partition; pragmatic gain is minor since the per-envelope cost is dominated by the prior parallel `from_xdr` decode.

### 2.17 Auto-restore `archived_prefetch.get(*k).cloned()` clone (residue of data_flows.md §2.18)

Mostly addressed via §3.5 (auto-restore Rc sharing): the bookkeeping now Rc-shares with the host-input vec, dropping from 3 clones to 1. The remaining one clone is `archived_prefetch.get(*k).cloned()` ([invoke.rs:470](src/rust/src/soroban_apply/invoke.rs#L470)) — the prefetch map stores typed `LedgerEntry`, and `cloned()` is a deep clone to lift it out for the auto-restore branch. The hot-archive prefetch entries can be sizable (CONTRACT_DATA / CONTRACT_CODE values).

The prefetch map can't easily move entries out (it's owned by the apply layer for the duration of the phase, and multiple TXs may auto-restore the same key). Storing `Rc<LedgerEntry>` in the prefetch map would let the auto-restore branch `Rc::clone` instead of deep clone. Worth tracking but not a SAC/Soroswap hot path.

### 2.18 Resources clone for the host (residue of data_flows.md §2.7 scope)

`apply_invoke_host_function` still calls `resources.clone()` to hand owned `SorobanResources` to the host ([invoke.rs:877](src/rust/src/soroban_apply/invoke.rs#L877)). The driver keeps `&SorobanResources` for the post-host delete-detection loop. The clone is dominated by cloning `footprint.read_only` / `read_write` (xvectors of `LedgerKey`).

Eliminating it requires destructuring `SorobanResources` and either moving the footprint into the host (then we lose post-host access) or having the host accept `&LedgerFootprint` (host-side API change to `build_storage_footprint_from_xdr`, deferred per §3.2 in the plan).

---

## 3. Resolved (for cross-reference with data_flows.md §2)

Items from the original §2 that are now fixed:

- §2.4 cost params encoded twice → `2882f78dc / d4a4059e2` (FastMap + drop duplicate encode).
- §2.5 `network_id` clone per TX → reverted (`e409f4100`); below-noise.
- §2.6 / §2.7 / §2.28 (auth / host_function / source_account / muxed-clone) → `968786713` (consume envelope).
- §2.10 / §2.11 storage map clone + try_unwrap → diagnosis uncertain; flagged in §2.3/§2.4 above for empirical follow-up.
- §2.12 RO TTL bumps re-encoded → `d125bca85` (bytes flow through `ro_ttl_bumps`).
- §2.15 phase-end double `key.to_xdr()` + double SHA-256 → `e5f4e4633` (precomputed `TtlKeyHash` through commit) + `71d0a1379` (cache rekeyed by `TtlKeyHash`).
- §2.16 partial: precomputed `TtlKeyHash` threading on the commit + Rc-cache paths.
- §2.17 RO `CONTRACT_DATA` deep clone every read → `42b3e6b93` (Rc cache extended to ContractData).
- §2.18 auto-restore three-way clone → `3248d47e4` (Rc shared between bookkeeping + host input).
- §2.19 hot/live restore decoded twice on C++ side → `325f7e321` (single decode + `PerTxDecodedRestores` plumbed through).
- §2.27 cap-check re-encode for shadowed reads → `afe176354` (host_bytes size for cluster-local shadow; cross-stage residue tracked above).
- §2.30 host's per-RO-slot encode + SHA-256 → already short-circuits via the `Option<TtlEntry>` path; no change needed.
- §2.32 `state_entry_rc_cache` keyed by LedgerKey → `71d0a1379` (rekeyed by `TtlKeyHash`).
- §2.2 prefetch per-byte `push_back` → `3318b2171` (`CxxBuf`-based `LedgerEntryInput`).

Items intentionally out of scope or skipped in this pass:
- §2.1 envelope round-trip — requires typed-input bridge.
- §2.3 classic prefetch encode-then-decode — same; would benefit from a typed-input bridge.
- §2.13 / §2.20 / §2.21 RestoreFootprint / ExtendFootprintTtl — user-deferred.
- §2.22 `compute_contract_code_size_for_rent` — fires only on new CONTRACT_CODE writes (0–2/phase).
- §2.23 / §2.24 diagnostic + contract event encode/decode — touches meta wire format.
- §2.25 envelope-size second walk — different semantics for fee-bump.
- §2.29 prefetch wire format (key from entry) — asymmetric per-entry cost.
- §2.31 success preimage hash — listed for completeness only, already moved to Rust.
