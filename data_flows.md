# Soroban parallel‑apply data flows (p26 path)

Scope: the Rust‑owned Soroban apply phase under [src/rust/src/soroban_apply/](src/rust/src/soroban_apply/), the cxx bridge in [src/rust/src/bridge.rs](src/rust/src/bridge.rs), the C++ glue in [src/ledger/LedgerManagerImpl.cpp](src/ledger/LedgerManagerImpl.cpp), and the p26 host call into [src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs). Older pinned hosts (p21–p25) and the bytes fallback in `invoke_host_function_old_env_serialized` are out of scope.

The shorthand used below for the data‑moving steps:

- **clone** — a Rust `.clone()` of a typed value (a `LedgerEntry` clone is a deep tree walk; an `Rc::clone` is one refcount bump and is called out separately).
- **enc** — `xdr::xdr_to_opaque` / `to_xdr` / `metered_write_xdr`: serializing typed values to XDR bytes.
- **dec** — `xdr::xdr_from_opaque` / `from_xdr` / `metered_from_xdr_with_budget`: parsing XDR bytes to typed values.
- **sha** — a SHA‑256 (used here for the TTL key hash and for the InvokeHostFunction success preimage).
- **memcpy** — bytewise copy of a byte buffer (`rust::Vec<u8>` push_back, `slice::to_vec`, etc.) when ownership crosses an API boundary that won't take a move.

---

## 1. Data flows

### 1.1 `TxSetFrame` → Rust envelope decode

| Step | Where | Operation |
|---|---|---|
| Wire bytes received from overlay are XDR‑decoded into the C++ `TransactionEnvelope` tree inside each `TransactionFrameBase`; that tree is now owned by the txset. | overlay → herder → `ApplicableTxSetFrame` | dec |
| `applyTransactions` → `applyParallelPhase` → `applySorobanPhaseRust` calls `phase.getParallelStages()` and gathers `&tx->getEnvelope()` pointers per cluster in apply order. | [LedgerManagerImpl.cpp:3303–3361](src/ledger/LedgerManagerImpl.cpp#L3303-L3361) | borrow only |
| For each envelope pointer, `toCxxBuf(*env)` = `xdr::xdr_to_opaque(env)` packs the entire envelope tree into a fresh `std::vector<uint8_t>` (`CxxBuf`). Fanned across 8 worker threads when ≥ 256 envelopes. | [LedgerManagerImpl.cpp:3362–3404](src/ledger/LedgerManagerImpl.cpp#L3362-L3404) | enc (per‑tx) |
| `rust::Vec<CxxBuf>` crosses the FFI by reference. | bridge | none (the CxxBuf wraps a `UniquePtr<CxxVector<u8>>`, so the data buffer itself is not copied) |
| `decode_envelopes_parallel` allocates a `Vec<Option<TransactionEnvelope>>` of size N and dispatches up to 8 workers; each worker `TransactionEnvelope::from_xdr(buf, Limits::none())` for its slice. Final `into_iter().map(unwrap).collect()` materializes the `Vec<TransactionEnvelope>`. | [orchestrator.rs:740–804](src/rust/src/soroban_apply/orchestrator.rs#L740-L804) | dec (per‑tx, parallel) |

Per‑envelope cost across the bridge: one **enc** in C++ + one **dec** in Rust. The same conceptual content (an `xdr::TransactionEnvelope`) exists in three representations during this window: the C++ tree (xdrpp types in the TxSetFrame), a transient encoded `CxxBuf`, and the Rust `TransactionEnvelope` tree consumed by the apply phase.

### 1.2 Per‑TX dispatch and field extraction

| Step | Where | Operation |
|---|---|---|
| `extract_tx_parts(envelope)` returns `(&MuxedAccount, &[Operation], &SorobanTransactionData)`. | [common.rs:516–542](src/rust/src/soroban_apply/common.rs#L516-L542) | borrow only |
| Soroban TX is required to be a single‑op TX. The Operation is destructured into `OperationBody::InvokeHostFunction(inv_op)` / `ExtendFootprintTtl(ext_op)` / `RestoreFootprint(_)`. | [orchestrator.rs:584–693](src/rust/src/soroban_apply/orchestrator.rs#L584-L693) | borrow only |
| `archivedSorobanEntries` (V_1 extension) → `Vec<u32>` via `ext.archived_soroban_entries.as_vec().clone()`. | [orchestrator.rs:628–634](src/rust/src/soroban_apply/orchestrator.rs#L628-L634) | clone (small Vec<u32>) |
| Per‑TX PRNG seed: `derive_per_tx_prng_seed(base_seed, tx_num)` does `SHA256(base_seed || tx_num.to_be_bytes())`. | [common.rs:568–573](src/rust/src/soroban_apply/common.rs#L568-L573) | sha (per‑tx) |
| `has_test_internal_error_memo(envelope)` is checked first; a BUILD_TESTS hook that bails before the host runs. | [common.rs:550–563](src/rust/src/soroban_apply/common.rs#L550-L563) | borrow only |

### 1.3 Classic / archived prefetch

| Step | Where | Operation |
|---|---|---|
| For every Soroban TX in the phase, walk RO+RW footprints; for each non‑Soroban key, `ltx.loadWithoutRecord(key)` → push `(key, entry.current())` into a `std::vector<std::pair<LedgerKey, LedgerEntry>>`. Two `entry.current()` reads pull a fresh `LedgerEntry` from `LedgerTxn`'s internal map. | [LedgerManagerImpl.cpp:1538–1580](src/ledger/LedgerManagerImpl.cpp#L1538-L1580) | clone (per key, classic only) |
| Parallel encode (≥ 256 entries, 8 workers): `appendPrefetchEntry` does `xdr::xdr_to_opaque(key)` + `xdr::xdr_to_opaque(entry)` into a `LedgerEntryUpdate{ key_xdr, value_xdr }`. The inner `rust::Vec<u8>` is filled by **per‑byte `push_back`** (no bulk‑copy API exposed by cxx). | [LedgerManagerImpl.cpp:1507–1638](src/ledger/LedgerManagerImpl.cpp#L1507-L1638) | enc + memcpy (per key) |
| `buildArchivedPrefetchForPhase` snapshots the apply state's hot archive (`mApplyState.copyLedgerStateSnapshot`), gathers archive keys from RestoreFootprint RW footprints and InvokeHostFunction RO+RW persistent footprints, calls `snap.loadArchiveKeys(archiveKeys)`, and runs the same `appendPrefetchEntry` for each `HOT_ARCHIVE_ARCHIVED` bucket entry. | [LedgerManagerImpl.cpp:1658–1731](src/ledger/LedgerManagerImpl.cpp#L1658-L1731) | enc + memcpy (per key) |
| `build_prefetch_map(updates)` on the Rust side decodes each `(key_xdr, value_xdr)` back into typed `LedgerKey`/`LedgerEntry` and builds the `HashMap<LedgerKey, LedgerEntry>` the apply path queries. | [common.rs:499–512](src/rust/src/soroban_apply/common.rs#L499-L512) | dec (per key) |

Per‑classic‑prefetch entry: one **clone** out of `LedgerTxn`, **enc** of key + entry, **memcpy** byte‑by‑byte into `rust::Vec<u8>`, then **dec** of key + entry on the Rust side back into typed form.

### 1.4 `CxxLedgerInfo`, cost params, fees, prng seed

| Step | Where | Operation |
|---|---|---|
| `CxxLedgerInfo` is built once per phase. `cpu_cost_params` / `mem_cost_params` are encoded twice (once into `ledgerInfo` and once into separate `cpuCostParams` / `memCostParams` CxxBufs passed alongside). | [LedgerManagerImpl.cpp:3429–3455](src/ledger/LedgerManagerImpl.cpp#L3429-L3455) | enc × 2 (per phase) |
| `prngSeedBuf.data->assign(begin, end)` copies the 32‑byte base PRNG seed. | [LedgerManagerImpl.cpp:3489–3492](src/ledger/LedgerManagerImpl.cpp#L3489-L3492) | memcpy (32 bytes, per phase) |
| Per‑TX envelope size: a fresh pass over `applyStages` queries `tx->getResources(false, version).getVal(TX_BYTE_SIZE)` per TX. | [LedgerManagerImpl.cpp:3461–3485](src/ledger/LedgerManagerImpl.cpp#L3461-L3485) | (recomputed) |
| Cost params decode: inside the typed host wrapper, decoded once per worker thread via a `thread_local` cache keyed by `(ptr, len)`. | [soroban_proto_all.rs:291–325](src/rust/src/soroban_proto_all.rs#L291-L325) | dec (1×/thread/phase) |
| `LedgerInfo::try_from(&CxxLedgerInfo)` clones `network_id` (32‑byte `Vec<u8>` → `[u8;32]`) per TX. | [soroban_proto_any.rs:63–79](src/rust/src/soroban_proto_any.rs#L63-L79) | clone (per‑tx, 32 B) |

### 1.5 `SorobanState` storage shape

The canonical Soroban state is owned by Rust as `Box<SorobanState>` held across ledgers in `mApplyState`. It stores `LedgerEntry`s by `TtlKeyHash` (`[u8; 32]`):

- `contract_data: HashMap<TtlKeyHash, ContractDataEntry { ledger_entry, ttl_data, size_bytes }>`
- `contract_code: HashMap<TtlKeyHash, ContractCodeEntry { ledger_entry, ttl_data, size_bytes, xdr_size_bytes }>`
- `pending_ttls: HashMap<TtlKeyHash, TtlData>` (used only during ingestion)

Lookups go through `state.get(key) → Option<EntryRef<'_>>`:

- `LedgerKey::ContractData` / `ContractCode` → `EntryRef::Borrowed(&ledger_entry)` (no allocation; one **sha** of XDR(key) inside `ttl_key_hash_for`).
- `LedgerKey::Ttl` → `EntryRef::Owned`: synthesizes a fresh `LedgerEntry { last_modified_ledger_seq, data: Ttl(TtlEntry{ key_hash, live_until_ledger_seq }), ext: V0 }`. Allocation per call.

`has_ttl(key)` follows the same hashing path (one **sha**) and probes `pending_ttls` then `contract_data` then `contract_code`.

### 1.6 Layered read of a footprint entry

`layered_get(state, cross_stage, cluster_local, classic_prefetch, key)` returns `Option<Cow<'_, LedgerEntry>>`:

1. `cluster_local.get(key)` → `Cow::Borrowed` of the in‑progress write.
2. `cross_stage.get(key)` → `Cow::Borrowed` of an earlier stage's write.
3. For `ContractData`/`ContractCode`/`Ttl`: `state.get(key)` → `Cow::Borrowed` (data/code) or `Cow::Owned` (TTL, synthesized).
4. Otherwise: `classic_prefetch.get(key)` → `Cow::Borrowed`.

Lookups in (1) and (2) hash the `LedgerKey` itself via Rust's `HashMap` default hasher (SipHash). Lookups in (3) compute `ttl_key_hash_for(key)` which is **`SHA256(XDR(key))`** — i.e. each visit serializes the key and SHA‑256s it.

### 1.7 InvokeHostFunction — Rust side, building host inputs

For each footprint key (RO followed by RW) in `apply_invoke_host_function` ([invoke.rs:347–791](src/rust/src/soroban_apply/invoke.rs#L347-L791)):

| Step | Operation |
|---|---|
| Tombstone short‑circuit (cluster_local/cross_stage map probe). | borrow only |
| Auto‑restore branch: `archived_prefetch.get(*k).cloned()` if not in live state → full `LedgerEntry` **clone** from the prefetch map; then `(*k).clone()` and `entry_value.clone()` for the `auto_restored_data_writes` / `_ttl_writes` bookkeeping (one **clone** of key + entry, kept aside for the C++ post‑pass). | clone × 2–3 |
| Regular branch: `layered_get` → `Cow<LedgerEntry>`. Then TTL: `state.get_ttl_entry_by_hash(key_hash, k)` which clones a `TtlEntry { key_hash: Hash([u8;32]).clone(), live_until_ledger_seq }`. The TTL hash is reused from the cache‑probe above so it isn't recomputed. | sha (only when neither cluster_local nor cross_stage has the TTL); small clone |
| Per‑entry size cap: `cached_xdr_size_for` if state‑resident, else `xdr_serialized_size(&entry_cow)` = re‑encode. | enc (only for shadowed reads) |
| `Rc<LedgerEntry>` wrapping: state‑resident **CONTRACT_CODE** only is cached in `state_entry_rc_cache: HashMap<LedgerKey, Rc<LedgerEntry>>` per cluster, so subsequent TXs in the same cluster only pay `Rc::clone` + `LedgerKey::clone` (for the cache key). All other paths (CONTRACT_DATA, classic, shadowed reads) do `entry_cow.into_owned()`, which is a deep **clone** for `Cow::Borrowed` cases (the borrow points into `SorobanState`). | clone (per RO read of CONTRACT_DATA / classic); Rc::clone (per RO read of CONTRACT_CODE) |

After the loop, `typed_ledger_entries: Vec<(Rc<LedgerEntry>, Option<TtlEntry>, u32)>` is handed to the host. On the typed path:

- `op.host_function.clone()` — clones the entire `HostFunction` tree. For a `CreateContract*` this includes the WASM bytes for upload paths. For `InvokeContract` this includes the arg `ScVal` tree.
- `resources.clone()` — clones the `SorobanResources` tree (footprint = two `xvector<LedgerKey>`).
- `op.auth.to_vec()` — clones the entire `Vec<SorobanAuthorizationEntry>` (auth trees can be sizeable: `SorobanAuthorizationEntry::credentials::Address` carries a signature + args).
- `muxed_to_account_id(source_account)` — copies the 32‑byte Ed25519 pubkey.
- When `enable_diagnostics`: `typed_ledger_entries.clone()` is taken so the post‑host `append_core_metrics_for_invocation` can read entry sizes; this re‑clones the whole `Vec<(Rc<LedgerEntry>, …)>` (Rc::clone for each entry, so cheap per entry, but the Vec is allocated again).

### 1.8 Bridge: the typed host call

`invoke_host_function_typed` ([soroban_invoke.rs:20–54](src/rust/src/soroban_invoke.rs#L20-L54)) → `invoke_host_function_typed_via_curr_host` ([soroban_proto_all.rs:257–463](src/rust/src/soroban_proto_all.rs#L257-L463)):

1. Decode `ContractCostParams` once per thread (cached).
2. Build a `Budget` from the cost params (calls `cpu_params.clone()` / `mem_params.clone()` to feed into Budget creation).
3. `LedgerInfo::try_from(&CxxLedgerInfo)` — see 1.4.
4. `std::panic::catch_unwind` over the host call.
5. On success: `extract_rent_changes`, `compute_rent_fee`, then `extract_ledger_effects_typed(res.ledger_changes)?` (described below).

### 1.9 p26 host internals (`invoke_host_function_typed`)

[e2e_invoke.rs:449–547](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L449-L547):

| Step | Operation |
|---|---|
| `build_restored_key_set`: for each restored RW index, `rw_footprint.get(i).metered_clone(budget)` then `Rc::new` — a deep clone of the `LedgerKey` per restored RW slot. | clone (per restored slot) |
| `build_storage_footprint_from_xdr(&budget, resources.footprint)`: for each RW and RO key, `key.metered_clone(budget)` then `Rc::metered_new(...)`. So every footprint key is **cloned again** here, even though the caller's `resources.footprint` was already a clone made by `resources.clone()` in 1.7. | clone (per footprint key) |
| `build_storage_map_from_typed_ledger_entries`: for each `(Rc<LedgerEntry>, Option<TtlEntry>, u32)`, calls `ledger_entry_to_ledger_key(&le, budget)` ([e2e_invoke.rs:1139–1167](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L1139-L1167)) — this **walks the LedgerEntry and rebuilds a fresh `LedgerKey` by metered‑cloning the inner fields** (account_id / asset / contract / key / hash). Then `Rc::metered_new(key, …)` wraps it. So for every footprint slot the `LedgerKey` exists *twice* in the host: once in the footprint map (1.9 step 2) and once in the storage map. | clone (per entry) |
| Then `Rc::metered_new(ttl_entry_in, budget)` per slot (TTLs are small). | clone (small per slot) |
| `let init_storage_map = storage_map.metered_clone(budget)?` — clones the entire persistent (HAMT) `StorageMap` to keep an immutable initial snapshot for the post‑host diff. The map nodes themselves clone; the `Rc<LedgerEntry>` / `Rc<LedgerKey>` payloads are refcount‑bumped only. | clone (map structure, per‑tx) |
| `host.set_*` for source_account / ledger_info / auth_entries / base_prng_seed (auth_entries is moved in; ledger_info and source_account are moved or metered‑cloned by the host's internal setter). | move/clone |
| `host.invoke_function(host_function)` runs the contract. | (contract work — out of scope) |
| `get_ledger_changes`: for every entry in `storage.map` (footprint size), `metered_write_xdr(budget, key.as_ref(), &mut encoded_key)` — **re‑encode the LedgerKey** even if it was just decoded / read out of state. For each RW slot with a new value, `metered_write_xdr(budget, entry.as_ref(), &mut entry_buf)` — **encode the new entry**. For each RO slot, no entry re‑encode but a SHA‑256 over the encoded key if no TTL was supplied (used as `key_hash`). When `init_xdr_sizes` is supplied (typed path), the old‑entry size is read from the BTreeMap and no `metered_write_xdr(old_entry, ...)` happens. | enc (per RW: key + new value); enc (per RO: key only); sha (per RO without TTL) |
| For each RW change, `entry_change.typed_new_value = Some(entry.clone())` — `Rc::clone`. | Rc::clone |
| `encode_contract_events`: for each emitted contract event, encode to bytes via the host metering path. | enc (per event) |
| `metered_write_xdr(&budget, &res, &mut encoded_result_sc_val)` encodes the host's `ScVal` return. | enc |
| Diagnostic events get encoded inside `encode_diagnostic_events` only when `enable_diagnostics`. | enc (per diag event) |

After the host returns, `extract_ledger_effects_typed` ([soroban_proto_all.rs:183–225](src/rust/src/soroban_proto_all.rs#L183-L225)) walks each `LedgerEntryChange`:

- For RW writes: `Rc::try_unwrap(typed).unwrap_or_else(|rc| (*rc).clone())`. The `typed_new_value` is shared between the storage map and the change record; the `try_unwrap` succeeds only if both copies have been dropped beforehand. In practice the storage map still holds a reference at this point, so this is a **clone** for every RW write. A typed `(LedgerEntry, RustBuf)` pair is pushed.
- For TTL changes: a fresh `LedgerEntry` is built from `(key_hash, new_live_until_ledger)` and then `non_metered_xdr_to_rust_buf(&le)` encodes it. Net: clone + enc per TTL bump.

### 1.10 Folding host output back into the cluster

Back in `apply_invoke_host_function` ([invoke.rs:937–1136](src/rust/src/soroban_apply/invoke.rs#L937-L1136)):

| Step | Operation |
|---|---|
| Write‑bytes / per‑entry size cap check uses `encoded.len()` directly (no re‑encode). | borrow only |
| For each `(owned, encoded)` in `modified_ledger_entries`: `owned.last_modified_ledger_seq = ledger_seq` and `patch_last_modified_seq(&mut encoded, ledger_seq)` (4‑byte memcpy). `ledger_entry_key(&owned)` walks the entry and clones inner fields to build a typed `LedgerKey` (same shape as the host's `ledger_entry_to_ledger_key` — so the LedgerKey for this entry is now constructed **a third time** in this TX: once in the original footprint, once in `build_storage_map_from_typed_ledger_entries`, and once here). | clone (per RW entry); 4‑byte memcpy |
| `build_tx_delta_with_cached_new` (only when `enable_tx_meta`): `key.to_xdr()` (enc) + previous: `layered_get` then `prev.to_xdr()` (enc) + new: `cached.to_vec()` (memcpy of the host‑supplied encoded bytes into a fresh `Vec<u8>`). Net per delta: 2 enc + 1 memcpy. | enc × 2, memcpy |
| For tombstones (RW keys missing from host output): `build_tx_delta(..., None)` — same as above but new is empty. The previous value is re‑encoded from `layered_get`. | enc × 2 |
| `host_bytes.insert(key.clone(), encoded)` and `cluster_local_writes.insert(key, Some(owned))` for RW writes. The `LedgerKey` is cloned here so the host_bytes map can be keyed independently from cluster_local_writes (key is consumed by the insert). | clone (LedgerKey, per RW entry) |
| Read‑only TTL bump: same shape, but stashed in `ro_ttl_bumps` and `host_bytes` is **not** populated (so the phase‑end emit will re‑encode the bumped TTL). | (per RO TTL bump: re‑enc later in 1.12) |

The post‑host `append_core_metrics_for_invocation` (only when `enable_diagnostics`) builds 19 `DiagnosticEvent`s and `to_xdr()`‑encodes each. It also re‑encodes each footprint key (`k.to_xdr()`) and each modified entry's derived key (`ledger_entry_key(entry).to_xdr()`) to count key bytes. Of the 19 events, several read the metric values produced by the loop.

`compute_success_preimage_hash` ([invoke.rs:1371–1389](src/rust/src/soroban_apply/invoke.rs#L1371-L1389)) hashes the return‑value bytes + 4‑byte event count + each event's encoded bytes into a 32‑byte SHA‑256, which is then handed to C++ in `success_preimage_hash`. No re‑decode.

`compute_refundable_fee_increment` ([common.rs:251–307](src/rust/src/soroban_apply/common.rs#L251-L307)) walks the typed footprint to count disk‑read entries and calls the per‑protocol `compute_transaction_resource_fee` adapter — borrow‑only over the footprint, but it does construct a `CxxTransactionResources` struct.

### 1.11 ExtendFootprintTtl, RestoreFootprint

`apply_extend_footprint_ttl` ([extend.rs](src/rust/src/soroban_apply/extend.rs)):

- For each RO footprint key: `layered_get` for the data entry (Cow), then `xdr_serialized_size(&data_entry)` — **always re‑encodes** to compute the size cap (no cached‑size fast path here), then `layered_get(&ttl_key)`. TTL data‑entry pairs are kept as `Cow<LedgerEntry>`.
- `extend_footprint_ttl_old_env` walks the slots, builds `CxxLedgerEntryRentChange`s, and for each clones the TTL: `slot.ttl_entry.clone()` then mutates `last_modified_ledger_seq` and `live_until_ledger_seq`. So **one full `LedgerEntry::clone()` per slot** to produce the new TTL.
- For protocol ≥ 23 + CONTRACT_CODE, `compute_contract_code_size_for_rent` does `entry.to_xdr()` + `cc.to_xdr()` (a second encode of just the ContractCodeEntry inner) + a call into the per‑protocol `contract_code_memory_size_for_rent_bytes` (decodes it again on the inside).
- `fold_extended_ttls`: per bumped TTL, `build_tx_delta(..., Some(&ttl_entry))` — re‑encodes key + prev + new (3 enc). Then `ro_ttl_bumps.insert(key, ttl_entry)` (no host_bytes path — phase‑end emit will re‑encode again, see 1.12).

`apply_restore_footprint` ([restore.rs](src/rust/src/soroban_apply/restore.rs)):

- `plan_restore_sources` walks RW footprint. For each restorable slot: `entry.into_owned()` (deep clone when state‑backed) or `archived_prefetch.get(k).cloned()` (deep clone from prefetch). Each `k.clone()` too.
- `restore_footprint_old_env` clones the entry again: `slot.source` matches yield `e.clone()`, bumps `last_modified_ledger_seq`, then for code entries on V_23+ does `compute_contract_code_size_for_rent` (= one more `entry.to_xdr()` + one more `cc.to_xdr()`).
- `fold_restored_entries`: per slot, two `LedgerEntryUpdate`s (data + TTL) get pushed into `hot_archive_restores` / `live_restores`, each with `key.to_xdr()` and `source_entry.to_xdr()` / `ttl_entry.to_xdr()`. Then `tx_changes.push(build_tx_delta(...))` (3 enc per delta) and `cluster_local_writes.insert(key, Some(entry.clone()))` for the data‑side of hot‑archive restores (`entry.clone()` is yet another deep clone, the third copy of this entry).

### 1.12 Phase‑end commit (`apply_phase_writes_to_state`)

[orchestrator.rs:222–418](src/rust/src/soroban_apply/orchestrator.rs#L222-L418):

`AccumulatedWrites: HashMap<LedgerKey, Option<LedgerEntry>>` is drained into four `Vec<(LedgerKey, Option<LedgerEntry>)>` buckets (data / code / TTL / classic). The `accumulated_host_bytes: HashMap<LedgerKey, Vec<u8>>` carries host‑supplied bytes for the entries whose latest write came from `invoke_host_function`'s typed output (RW writes only — RO TTL bumps and RestoreFootprint / ExtendFootprintTtl outputs are never in this map).

For each bucket and each entry the `push_update` helper:

| Step | Operation |
|---|---|
| `key.to_xdr(Limits::none())` for the key. | enc (per write) |
| If `host_bytes_cache.remove(key)` hits: reuse those bytes verbatim. Otherwise `e.to_xdr()` re‑encodes the entry. | enc (only when no host bytes — TTL bumps, Restore/Extend output, classic side effects) |
| `state.upsert_contract_data_with_size(entry, size)` / `upsert_contract_code(entry, size)`: moves `entry` in by‑move; internally `ledger_entry_key(&ledger_entry)` rebuilds the key (cloning inner fields), then `ttl_key_hash_for(&lk)` does `SHA256(XDR(key))`. So per write: one `LedgerKey` reconstruction (clone of inner fields) + one **enc** + one **sha**, even though the typed entry has already been keyed by the orchestrator. | clone, enc, sha (per write) |

Net per RW data/code write whose host bytes are cached: at apply_phase_writes_to_state we still do `key.to_xdr()` (enc) + `upsert`'s internal `ledger_entry_key()` + `XDR(key)` + `sha`. That's two encodes of the same key from different code paths.

TTL writes drained at this stage also re‑encode the bumped TTL (no host_bytes path was populated for TTLs).

### 1.13 Returning to C++ — bridge marshaling and consumption

The bridge struct returned across FFI:

```rust
SorobanPhaseResult {
    per_tx: Vec<SorobanTxApplyResult>,
    soroban_init_updates: Vec<LedgerEntryUpdate>,   // (key_xdr, value_xdr)
    soroban_live_updates: Vec<LedgerEntryUpdate>,
    soroban_dead_updates: Vec<LedgerEntryUpdate>,   // value_xdr empty
    classic_updates: Vec<LedgerEntryUpdate>,
}

SorobanTxApplyResult {
    success, is_internal_error, is_*_exceeded, …
    return_value_xdr: RustBuf,
    contract_events: Vec<RustBuf>,
    diagnostic_events: Vec<RustBuf>,
    tx_changes: Vec<LedgerEntryDelta {key,prev,new}>,
    hot_archive_restores: Vec<LedgerEntryUpdate>,
    live_restores: Vec<LedgerEntryUpdate>,
    success_preimage_hash: RustBuf,
    refundable_fee_increment, rent_fee_consumed, …
}
```

Each `RustBuf` and `Vec<u8>` is moved across the FFI by ownership transfer (no copy in the marshaling itself; cxx hands the heap pointer/length).

On the C++ side ([LedgerManagerImpl.cpp:3517–3705](src/ledger/LedgerManagerImpl.cpp#L3517-L3705)):

| Consumer | Operation |
|---|---|
| `parallelDecodeEntries(result.soroban_init_updates)` and same for `live_updates`: parallel XDR decode each `value_xdr` into a `LedgerEntry`, then sequentially `ltx.createWithoutLoading(InternalLedgerEntry(std::move(entry)))` / `updateWithoutLoading`. The key is reconstructed by LedgerTxn from the entry (no separate key decode). | dec (per entry); move into ltx |
| `soroban_dead_updates`: per update, `xdr_from_opaque(update.key_xdr.data, key)` then `ltx.eraseWithoutLoading(key)`. | dec (per key) |
| `classic_updates`: per update, `xdr_from_opaque(key_xdr)` + (if not empty) `xdr_from_opaque(value_xdr)` and `ltx.load/create`. | dec (per key + per non‑deletion entry) |
| Hot‑archive / live restores: walk all per_tx `hot_archive_restores` / `live_restores`. For each entry, decode the value (`xdr_from_opaque`), pair data with TTL by `getTTLKey(e).ttl().keyHash`, then `ltx.markRestoredFromHotArchive(dataEntry, ttlEntry)` / `markRestoredFromLiveBucketList(...)`. Both data and TTL entries are decoded again on the C++ side. | dec (per restore entry) |
| `processSorobanPerTxResult` for each TX (only the parts that touch meta when meta is enabled): |  |
| ↳ diagnostic events: `xdr_from_opaque` per event. | dec (per diag event) |
| ↳ contract events: `xdr_from_opaque` per event, only when meta enabled. | dec (per event) |
| ↳ return value: `xdr_from_opaque` once for `setSorobanReturnValue`. The preimage hash is read straight as 32 raw bytes — no second hash on the C++ side. | dec |
| ↳ `tx_changes`: per delta, decode `key_xdr`, optionally `prev_value_xdr`, optionally `new_value_xdr`, build `LedgerEntryChanges`. | dec × up to 3 per delta |
| ↳ `hot_archive_restores` / `live_restores` are decoded AGAIN inside `processSorobanPerTxResult` to build `hotArchiveRestores` / `liveRestores` `UnorderedMap<LedgerKey,LedgerEntry>` for `processOpLedgerEntryChanges`. (Same byte buffers are decoded both here and in the markRestored loop above.) | dec (second pass per restore entry) |

`success_preimage_hash` is consumed via raw `memcpy` (32 bytes) into `opResult.tr().invokeHostFunctionResult().success()`.

### 1.14 Long‑lived `SorobanState` mutation path

After `apply_phase_writes_to_state`, the bridge result's init/live/dead/classic updates are routed through `ltx`; SorobanState itself is mutated in‑place during the phase‑end pass (`upsert_contract_data_with_size` / `upsert_contract_code` / `create_ttl` / `update_ttl` / `delete_*`). Outside the apply phase, the SorobanState exposes XDR‑in / XDR‑out FFI methods for the BucketTestUtils replay path, ledger‑close apply hooks, and post‑apply eviction notifications. The `_xdr` family does a fresh decode of every input buffer and a fresh encode of every output — used outside the hot apply path so it's listed only for completeness.

---

## 2. Identified inefficiencies

Items below are categorized as **clone**, **encode/decode**, or **hash** wins where applicable. Lines like "P=high/med/low" rank rough magnitude based on per‑phase counts.

### 2.1 Envelope round‑trip across the bridge

(enc + dec, P=high) [src/ledger/LedgerManagerImpl.cpp:3340–3404](src/ledger/LedgerManagerImpl.cpp#L3340-L3404), [src/rust/src/soroban_apply/orchestrator.rs:740–804](src/rust/src/soroban_apply/orchestrator.rs#L740-L804). For every Soroban TX the C++ side `xdr::xdr_to_opaque(env)` the envelope tree that was already a fully decoded structure inside `TxSetFrame`, and Rust then `from_xdr`'s the same bytes back into the canonical `TransactionEnvelope`. Both sides parallelize over ≤ 8 threads, but the total work (per‑byte serialize/deserialize of every envelope, ~5 µs/TX one way) is pure marshaling. The C++ envelope tree is destroyed after the call. Both representations are nominally the same XDR shape; the FFI does not require this round‑trip in principle.

### 2.2 Classic prefetch byte‑by‑byte push into `rust::Vec<u8>`

(memcpy, P=med) [LedgerManagerImpl.cpp:1507–1638](src/ledger/LedgerManagerImpl.cpp#L1507-L1638). `appendPrefetchEntry` uses `for (auto b : keyBytes) u.key_xdr.data.push_back(b)` — single‑byte pushes into a `rust::Vec<u8>` because cxx doesn't expose a bulk insert. The same code path is used for hot‑archive prefetch.

### 2.3 Classic prefetch encode‑then‑decode

(enc + dec, P=med) Same path: each classic LedgerEntry loaded from `ltx` is encoded to XDR (`xdr_to_opaque`) and then decoded by Rust (`build_prefetch_map`). The two ends live in the same process and the classic LedgerEntry is already in‑memory in C++. For ~few thousand source‑account / trustline reads per phase this is non‑trivial.

### 2.4 Cost params encoded twice into `CxxLedgerInfo` and separate buffers

(enc, P=low/per‑phase) [LedgerManagerImpl.cpp:3443–3452](src/ledger/LedgerManagerImpl.cpp#L3443-L3452). `toCxxBuf(sorobanConfig.cpuCostParams())` and `toCxxBuf(memCostParams())` are called twice each: once into `ledgerInfo.cpu_cost_params` / `mem_cost_params`, once into the standalone `cpuCostParams` / `memCostParams` arguments to `apply_soroban_phase`. The Rust side never reads `ledgerInfo.cpu_cost_params` / `mem_cost_params` from the typed invoke path (it goes through the bare params), so the LedgerInfo embed is a wasted encode of two non‑trivial XDR trees per phase.

### 2.5 `network_id` cloned per TX

(clone, P=low) [src/rust/src/soroban_proto_any.rs:70](src/rust/src/soroban_proto_any.rs#L70). `LedgerInfo::try_from(&CxxLedgerInfo)` runs per TX and copies the 32‑byte network ID; could be hoisted to phase scope.

### 2.6 `auth_entries.to_vec()` and `op.auth.to_vec()`

(clone, P=med per‑tx with auth) [src/rust/src/soroban_apply/invoke.rs:817](src/rust/src/soroban_apply/invoke.rs#L817). The auth Vec is cloned out of the typed op even though the op itself is a borrow that lives at least as long as the host call. The host takes the auth Vec by‑value; this is a single mandatory clone but the contents (`SorobanAuthorizationEntry` with credentials / args) could potentially be moved out if the op itself were owned.

### 2.7 `op.host_function.clone()` + `resources.clone()` per TX

(clone, P=med per tx) [invoke.rs:843–844](src/rust/src/soroban_apply/invoke.rs#L843-L844). The current driver clones the HostFunction (which for `CreateContract` upload paths includes WASM bytes) and the SorobanResources (which carries the footprint as `xvector<LedgerKey>`) so it can hand owned values to the typed host call. The typed‑input ABI accepts owned values; the orchestrator could hand them in by‑move from the parsed envelope rather than borrowing from `&op` (`tx_envelope` could be consumed instead of borrowed).

### 2.8 LedgerKey rebuilt three times per RW write

(clone, P=high) The same Soroban RW LedgerKey is materialized in at least three distinct typed instances during one TX: (i) the host's `build_storage_footprint_from_xdr` clone, (ii) the host's `build_storage_map_from_typed_ledger_entries` rebuild via `ledger_entry_to_ledger_key`, and (iii) the Rust driver's `ledger_entry_key(&owned)` to derive the key from the modified entry after the host returns. Beyond that, `host_bytes.insert(key.clone(), ...)` and `cluster_local_writes.insert(key, ...)` themselves split the key into two clones for storage. The host could reuse the footprint's `Rc<LedgerKey>` for the storage map (the typed call already has it on hand), and the post‑host loop could index by the position in `modified_ledger_entries` to look up the key the host already produced (`encoded_key`).

### 2.9 `ledger_entry_to_ledger_key` deep‑clones inner fields

(clone, P=high) [e2e_invoke.rs:1139–1167](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L1139-L1167). To turn a `&LedgerEntry` into a `LedgerKey` the host metered‑clones each inner field (`account_id`, `asset`, `contract`, `key`, `hash`). For CONTRACT_DATA the `key` SCVal can be sizable. The footprint's own key would already match — see 2.8.

### 2.10 `storage_map.metered_clone(budget)` per TX

(clone, P=high) [e2e_invoke.rs:485](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L485). The host clones the entire initial storage map to keep an immutable snapshot for the post‑host diff. The HAMT structure clones; the `Rc<LedgerEntry>` payloads only refcount‑bump. Footprints of N entries cost O(N) tiny allocations per TX. Holding the snapshot's `Rc<LedgerEntry>`s pinned is also what forces 2.11.

### 2.11 `Rc::try_unwrap` falls back to clone in `extract_ledger_effects_typed`

(clone, P=high) [src/rust/src/soroban_proto_all.rs:199–201](src/rust/src/soroban_proto_all.rs#L199-L201). The intent is to move the typed `LedgerEntry` out of the storage map's `Rc`, but the host has just cloned the storage map (2.10) so there are always at least two refcounts when this runs. The `unwrap_or_else(|rc| (*rc).clone())` therefore always clones. Dropping the `init_storage_map` before this loop (e.g. by computing the diff in two passes, or scoping the snapshot's lifetime smaller) would let `try_unwrap` succeed.

### 2.12 RO TTL bumps re‑encoded at phase end

(enc, P=med) [invoke.rs:1030–1052](src/rust/src/soroban_apply/invoke.rs#L1030-L1052), [orchestrator.rs:371–393](src/rust/src/soroban_apply/orchestrator.rs#L371-L393). The host returns TTL changes already encoded in `encoded_new_value`, but the invoke driver explicitly skips populating `host_bytes` for RO TTL bumps (because cluster‑end max‑merging can drop the bytes for a losing bump). The phase‑end commit then re‑encodes the kept TTL entry. TTLs are ~40 bytes each so the per‑entry cost is small, but for phases with many RO TTL bumps this is N encodes that could be avoided by also buffering the bytes alongside the typed entries.

### 2.13 ExtendFootprintTtl / RestoreFootprint produce no `host_bytes`

(enc, P=med) Same idea but in the ExtendFootprintTtl / RestoreFootprint drivers: every TTL bump and every restored entry/TTL is re‑encoded inside `apply_phase_writes_to_state`'s `push_update` because nothing populates `host_bytes`. The drivers already build typed entries; encoding them once and threading the bytes through would skip the second encode.

### 2.14 Tx delta re‑encodes prev and key

(enc, P=med, only with meta enabled) [common.rs:463–494](src/rust/src/soroban_apply/common.rs#L463-L494). `build_tx_delta_with_cached_new` re‑encodes the `LedgerKey` and the previous entry (the latter via `layered_get` → `.to_xdr()`) on every TX delta. The key was just encoded by `apply_phase_writes_to_state` (or by `push_update`); the prev entry was either still in `SorobanState` (we already have its bytes available implicitly because it was decoded once into storage) or in another write layer. The drivers cannot easily reuse bytes for prev (state holds typed, not bytes), but the key encoding is a clear duplicate.

### 2.15 `apply_phase_writes_to_state` encodes key, then upsert encodes again

(enc + sha, P=high) [orchestrator.rs:261–284](src/rust/src/soroban_apply/orchestrator.rs#L261-L284), [state.rs:430–467](src/rust/src/soroban_apply/state.rs#L430-L467). For every Soroban write at phase end: `push_update` does `key.to_xdr()` to build the LedgerEntryUpdate's `key_xdr`, then `state.upsert_contract_data_with_size(entry, size)` rebuilds the LedgerKey from the entry and re‑encodes it inside `ttl_key_hash_for` to compute the SHA‑256 hash. So per write: two `to_xdr()` calls on (effectively) the same key, plus a `SHA256`.

### 2.16 `ttl_key_hash_for(key)` re‑hashes on every access

(enc + sha, P=high) [common.rs:92–100](src/rust/src/soroban_apply/common.rs#L92-L100). The hash is `SHA256(XDR(key))`. Every `state.get` / `state.has_ttl` / `state.upsert_*` / `state.delete_*` / `state.create_ttl` / `state.cached_xdr_size_for` calls it. For a phase with thousands of footprint slots, each slot is hashed at least 2–3 times (footprint walk, RW write back, possibly auto‑restore lookup, possibly tombstone walk). Caching the key→hash on the `LedgerKey` itself (or threading the precomputed hash through the call chain — `state.get_ttl_entry_by_hash` already takes a `TtlKeyHash`) would let one hash per phase‑lifetime suffice.

### 2.17 `LedgerEntry::clone()` for RO CONTRACT_DATA reads

(clone, P=high) [invoke.rs:776–790](src/rust/src/soroban_apply/invoke.rs#L776-L790). The `state_entry_rc_cache` only covers `LedgerKey::ContractCode`; CONTRACT_DATA RO reads go through `entry_cow.into_owned()` which clones the entry tree out of `SorobanState` for every TX that touches the same RO data slot. Many Soroban workloads have a "hot" contract‑instance entry (e.g. the SAC's WASM stub data, an issuer authorization map) that is RO across hundreds of TXs in a cluster. Extending the Rc cache to ContractData would amortize this clone.

### 2.18 Auto‑restore branch double‑clones the entry

(clone, P=med per restored slot) [invoke.rs:543–584](src/rust/src/soroban_apply/invoke.rs#L543-L584). Hot‑archive auto‑restores `archived_prefetch.get(*k).cloned()` once, then push `entry_value.clone()` into `auto_restored_data_writes` and `Rc::new(entry_value)` into `typed_ledger_entries`. Three live copies of the same entry remain until the TX finishes. Could be reduced to one Rc‑wrapped copy + one bytes form (for the restore record).

### 2.19 Hot/live restore entries serialized then re‑decoded twice

(enc + dec ×2, P=med per restore) [invoke.rs:1340–1365](src/rust/src/soroban_apply/invoke.rs#L1340-L1365), [LedgerManagerImpl.cpp:3650–3700](src/ledger/LedgerManagerImpl.cpp#L3650-L3700), [LedgerManagerImpl.cpp:3218–3245](src/ledger/LedgerManagerImpl.cpp#L3218-L3245). The Rust side encodes each `(key, entry)` pair into a `LedgerEntryUpdate`, then C++ decodes them twice: once in `LedgerManagerImpl::applySorobanPhaseRust` to build `markRestoredFromHotArchive` calls, and again in `processSorobanPerTxResult` to feed `hotArchiveRestores` / `liveRestores` into `processOpLedgerEntryChanges`. The two decoders run over identical byte buffers.

### 2.20 RestoreFootprint clones entries three times

(clone, P=med per restore slot) [restore.rs:69–127](src/rust/src/soroban_apply/restore.rs#L69-L127). `plan_restore_sources` clones the source entry (from prefetch or state), `restore_footprint_old_env` clones the slot's entry again to bump `last_modified_ledger_seq`, then `fold_restored_entries` clones once more when inserting into `cluster_local_writes`. Three full deep clones of every restored entry.

### 2.21 ExtendFootprintTtl: re‑encode entries to size‑cap and to size‑for‑rent

(enc, P=med per slot) [extend.rs:178–199](src/rust/src/soroban_apply/extend.rs#L178-L199). `xdr_serialized_size(&data_entry)` re‑encodes the data entry for the cap check on every slot, even when the entry was just read from `SorobanState` where the size is cached. For CONTRACT_CODE on V_23+, `compute_contract_code_size_for_rent` re‑encodes the entry (`entry.to_xdr()`) AND the inner `ContractCodeEntry` (`cc.to_xdr()`) — and the per‑protocol `contract_code_memory_size_for_rent_bytes` decodes the inner buf yet again on the way in.

### 2.22 `compute_contract_code_size_for_rent` re‑encodes inner ContractCodeEntry

(enc + dec, P=med) [common.rs:171–199](src/rust/src/soroban_apply/common.rs#L171-L199). Both calls — the data entry's `to_xdr` (for `xdr_serialized_size`) and the inner ContractCodeEntry `to_xdr` (for the memory‑size bridge) — are full encodes. The bridge then decodes the inner buf again inside `contract_code_memory_size_for_rent_bytes`. Three steps to compute one number.

### 2.23 Diagnostic events encoded then sent as separate `RustBuf`s

(enc per event, P=high when diagnostics are on) [src/rust/src/soroban_apply/invoke.rs:85–113](src/rust/src/soroban_apply/invoke.rs#L85-L113), [invoke.rs:1258–1335](src/rust/src/soroban_apply/invoke.rs#L1258-L1335). Each of the 19 `core_metrics` diagnostic events is independently `to_xdr()`'d into its own `Vec<u8>` and shipped across the bridge as a `RustBuf` element; the C++ side then `xdr_from_opaque`'s every event back into a `DiagnosticEvent`. For phases with diagnostics enabled this is `19 × tx_count` encodes plus the same number of decodes. The metric values themselves are small u64s; bundling them into a single struct (rather than 19 individual events) would reduce both ends.

### 2.24 Contract events: encoded by host, decoded by C++

(dec, P=med, only when meta is enabled) The host already returns encoded events as `Vec<Vec<u8>>` and those bytes go straight into `result.contract_events`. C++'s `processSorobanPerTxResult` decodes each into a `ContractEvent` only to push it into `opMeta.getEventManager().setEvents()`. EventManager could be taught to take pre‑encoded bytes directly (or the encode could be deferred until meta finalize). Diagnostic events have the same pattern.

### 2.25 Per‑TX envelope size derived twice

(small, P=low) [LedgerManagerImpl.cpp:3461–3485](src/ledger/LedgerManagerImpl.cpp#L3461-L3485). C++ does a fresh `tx->getResources(...).getVal(TX_BYTE_SIZE)` per TX to build `perTxEnvelopeSizeBytes`, after the parallel envelope encoder in 2.1 already had the per‑TX byte length on hand (`encoded[i].data->size()`). Threading that length out of the encoder loop avoids the second walk.

### 2.26 `LedgerEntryUpdate` parallel decode then sequential ltx insert

(dec, P=med) [LedgerManagerImpl.cpp:3534–3608](src/ledger/LedgerManagerImpl.cpp#L3534-L3608). The C++ post‑pass parallel‑decodes Soroban init/live updates into `LedgerEntry`s, but those entries are immediately consumed by `ltx.createWithoutLoading/updateWithoutLoading`. The decode itself is the only parallelizable step (ltx is single‑writer). If Rust returned typed entries through the bridge (e.g. via a typed‑move handle), this decode could be skipped entirely — the typed entries are already what the host built before encoding. Same shape applies to the bridge marshaling of `tx_changes` (decoded again to build LedgerEntryChanges) and `classic_updates`.

### 2.27 SorobanState lookup → encode‑for‑host re‑encode

(enc, P=high) Background pattern: Rust holds `LedgerEntry` typed, then the apply path serializes it to give to the host inside `build_storage_map_from_typed_ledger_entries` *no* — that's actually the typed path's win, no serialize. But the cap‑check call site at [invoke.rs:728–736](src/rust/src/soroban_apply/invoke.rs#L728-L736) only uses `cached_xdr_size_for(k)` for unshadowed reads; shadowed reads do `xdr_serialized_size(&entry_cow)` = re‑encode. Cluster‑local writes already came with bytes in `host_bytes` (when emitted by the host); plumbing those bytes through would let the cap check avoid the second encode in the shadowed case.

### 2.28 `MuxedAccount → AccountId` clones the ed25519 key

(clone, P=low per‑tx) [common.rs:333–341](src/rust/src/soroban_apply/common.rs#L333-L341). 32 bytes per TX. Could move out of the parsed envelope rather than clone, given the envelope is dropped right after.

### 2.29 `build_prefetch_map` decodes both keys and entries

(dec, P=med) [common.rs:499–512](src/rust/src/soroban_apply/common.rs#L499-L512). The map is keyed by `LedgerKey`, but the C++ encoder always pairs (key, entry) and Rust always decodes both. For classic prefetch the key is recoverable from the entry (`LedgerEntryKey(le)`), so the on‑the‑wire format could drop the key entirely.

### 2.30 Duplicate hash of footprint keys in the host's `get_ledger_changes`

(sha + enc, P=med) [e2e_invoke.rs:221–232](src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs#L221-L232). For every footprint key with no TTL the host re‑encodes the key (`metered_write_xdr(key, &mut entry_change.encoded_key)`) and SHA‑256s the encoded form to fill `ttl_change.key_hash`. The Rust driver passes an `Option<TtlEntry>` per slot so the host generally has a TTL for Soroban keys, but slots with no live TTL (new persistent entries created within the TX) still hit this path. Pre‑hashing on the Rust side and passing the hash through would save the host's encode+sha.

### 2.31 `success_preimage` hashing in Rust + secondary hashing on TTL key

(sha, P=low/medium) [invoke.rs:1371–1389](src/rust/src/soroban_apply/invoke.rs#L1371-L1389). The preimage hash is computed bit‑by‑bit from already‑encoded bytes (no decode), so this one is fine. Listed for completeness because moving it to Rust is itself an optimization captured in the change history; the only further win is that the host's `encode_contract_events` already did one full pass over the same byte content, and the SHA‑256 could plausibly run during that encode rather than as a second pass over the bytes.

### 2.32 `Rc<LedgerEntry>` cache key is itself a cloned `LedgerKey`

(clone, P=low) [invoke.rs:778–786](src/rust/src/soroban_apply/invoke.rs#L778-L786). Inserting into `state_entry_rc_cache` does `state_entry_rc_cache.insert((*k).clone(), Rc::clone(&rc))`. Keying the cache by the SorobanState's `TtlKeyHash` ([u8;32]) instead would skip both the LedgerKey clone and the per‑lookup `Hash<LedgerKey>` (which itself is `Hash` over the cloned typed tree).

---

## Quick reference: per‑Soroban‑TX hot‑path operation counts

For an InvokeHostFunction TX with `r` RO + `w` RW footprint slots (Soroban + classic) and `e` emitted contract events, the work attributable to the apply pipeline (i.e. excluding the contract execution itself) is approximately:

- **enc:** envelope (1) + per‑footprint‑slot key during host `get_ledger_changes` (`r+w`) + per‑RW new value in host (`w`) + per‑event in host (`e`) + return‑value SCVal (1) + per‑modified‑entry post‑host key derivation (when delta meta is on, `~w` re‑encodes) + per‑bucket update emission (1 + 1 per write at phase end, sometimes hitting cached host bytes) + tx_changes prev (`~w` re‑encodes of prev entries when meta is on) + 19× metric events when diagnostics on.
- **dec:** envelope (1, on the Rust side); zero entry decodes for the typed host path; `~w` re‑decodes per RW update in C++ post‑pass; `~e` per‑event decodes when meta is enabled; `~3w` per‑delta decodes in `processSorobanPerTxResult` when meta is enabled.
- **clone:** auth Vec (1), host_function (1), resources (1), source ed25519 key (1), per‑footprint LedgerKey (host: 2× = `2(r+w)`), per‑entry LedgerEntry → LedgerKey rebuild in host (`r+w`), per‑RO data‑entry LedgerEntry clone out of state (1× per non‑code RO that's state‑backed and unshadowed = `~r_data`), storage_map structural clone (1, with `r+w` Rc bumps), `Rc::try_unwrap` clone per RW write (`w`), post‑host LedgerKey rebuild from modified entry (`w`), cluster_local_writes key insert (`w`).
- **sha:** per‑footprint‑slot `ttl_key_hash_for` call during read (`~r+w` for Soroban slots, multiple times each), per‑write `ttl_key_hash_for` during phase‑end upsert (`~w` for Soroban slots), per‑foot‑slot SHA‑256 in host `get_ledger_changes` for slots without TTL (`~r+w` worst case), success preimage hash (1).
