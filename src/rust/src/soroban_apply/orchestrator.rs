// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Soroban parallel-apply phase orchestration. Walks the TxSet's stages
//! and clusters, runs clusters in parallel via std::thread::scope,
//! merges per-cluster writes at the stage barrier, and folds the final
//! diffs into both `SorobanState` (for next-ledger reads) and the bridge
//! result (for bucket-list / LedgerTxn writeback).
//!
//! Mirrors the legacy C++ `LedgerManagerImpl::applySorobanStages` +
//! `ParallelApplyUtils.cpp`.

use sha2::{Digest, Sha256};

use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::{
    LedgerEntry, LedgerKey, Limits, MuxedAccount, OperationBody, ReadXdr,
    SorobanTransactionDataExt, TransactionEnvelope, WriteXdr,
};

use super::common::{
    build_prefetch_map, compute_contract_code_size_for_rent, copy_rent_fee_config,
    derive_per_tx_prng_seed, extract_tx_parts, extract_tx_parts_owned,
    has_test_internal_error_memo, merge_ttl_max, ttl_live_until_in_writes, ttl_live_until_of,
    ttl_lookup_key_for, AccumulatedWrites, FastMap, FxBuildHasher, TtlKeyHash,
};
use super::extend::apply_extend_footprint_ttl;
use super::invoke::apply_invoke_host_function;
use super::restore::apply_restore_footprint;
use super::state::SorobanState;
use crate::{
    CxxBuf, CxxLedgerInfo, CxxRentFeeConfiguration, LedgerEntryUpdate, RustBuf,
    SorobanModuleCache, SorobanPhaseResult,
};

pub fn apply_soroban_phase(
    state: &mut SorobanState,
    module_cache: &SorobanModuleCache,
    config_max_protocol: u32,
    soroban_envelopes: &Vec<CxxBuf>,
    soroban_cluster_sizes: &Vec<u32>,
    soroban_stage_cluster_counts: &Vec<u32>,
    soroban_base_prng_seed: &CxxBuf,
    classic_prefetch: &Vec<crate::LedgerEntryInput>,
    archived_prefetch: &Vec<crate::LedgerEntryInput>,
    ledger_info: &CxxLedgerInfo,
    rent_fee_configuration: CxxRentFeeConfiguration,
    per_tx_max_refundable_fee: &Vec<i64>,
    enable_diagnostics: bool,
    enable_tx_meta: bool,
    fee_configuration: crate::CxxFeeConfiguration,
    per_tx_envelope_size_bytes: &Vec<u32>,
) -> Result<SorobanPhaseResult, Box<dyn std::error::Error>> {
    let total_txs = soroban_envelopes.len();
    let envelopes = decode_envelopes_parallel(soroban_envelopes)?;
    let stages_idx = build_stage_cluster_indices(
        soroban_cluster_sizes,
        soroban_stage_cluster_counts,
        total_txs,
    )?;
    // Partition envelopes into per-cluster owned Vecs so each worker
    // can consume its TXs by-value and the per-TX driver can move
    // op.host_function / auth / source_account directly into the host
    // call without a clone. The split here uses the same indices the
    // workers iterate below; per_cluster[global_idx] aligns with
    // stage_clusters[i] for cluster_idx_global + i.
    let total_clusters: usize = stages_idx.iter().map(|s| s.len()).sum();
    let mut per_cluster_envelopes: Vec<Vec<TransactionEnvelope>> =
        Vec::with_capacity(total_clusters);
    {
        let mut iter = envelopes.into_iter();
        for stage_clusters in &stages_idx {
            for (begin, end) in stage_clusters {
                let size = end - begin;
                let mut chunk = Vec::with_capacity(size);
                for _ in 0..size {
                    chunk.push(iter.next().expect("partition: envelope underflow"));
                }
                per_cluster_envelopes.push(chunk);
            }
        }
    }

    let classic_prefetch_map = build_prefetch_map(classic_prefetch)?;
    let archived_prefetch_map = build_prefetch_map(archived_prefetch)?;

    // Pre-size accumulated_writes / host_bytes assuming ~4 writes per tx
    // (RW data + RW TTL on success) to avoid rehash churn during the
    // stage-barrier merge.
    let est_total_writes: usize = total_txs.saturating_mul(4);
    let mut accumulated_writes: AccumulatedWrites =
        FastMap::with_capacity_and_hasher(est_total_writes, FxBuildHasher::default());
    // Phase-level cache of host-supplied encoded bytes, keyed by
    // LedgerKey. Each cluster's own cache is folded in here at the
    // stage barrier; apply_phase_writes_to_state consumes the merged
    // result so it can hand the host's bytes straight to the
    // LedgerEntryUpdate without a re-serialize.
    let mut accumulated_host_bytes: FastMap<LedgerKey, Vec<u8>> =
        FastMap::with_capacity_and_hasher(est_total_writes, FxBuildHasher::default());
    let mut per_tx: Vec<crate::SorobanTxApplyResult> = Vec::new();

    // Per stage: run all clusters in parallel via std::thread::scope.
    // Workers borrow `&state`, `&accumulated_writes`, and the prefetch
    // maps from the orchestrator scope; their per-cluster local writes
    // come back through the join. The scope guarantees every worker has
    // joined before the borrow checker lets us mutate accumulated_writes
    // again — that's how the stage-barrier merge stays sound without an
    // Arc/RwLock.
    let state_ref: &SorobanState = state;
    let prng_seed_bytes: &[u8] = soroban_base_prng_seed.as_ref();
    // Pre-bound references the worker `move` closures can capture by
    // (Copy) reference rather than moving the underlying HashMap /
    // CxxRentFeeConfiguration values out of the outer scope.
    let classic_prefetch_ref = &classic_prefetch_map;
    let archived_prefetch_ref = &archived_prefetch_map;
    let rent_fee_ref = &rent_fee_configuration;
    let fee_config_ref = &fee_configuration;
    let env_sizes_ref: &Vec<u32> = per_tx_envelope_size_bytes;
    let mut next_tx_num: u64 = 0;
    let mut cluster_idx_global: usize = 0;
    for stage_clusters in &stages_idx {
        let cross_stage_ref: &AccumulatedWrites = &accumulated_writes;
        // Pre-compute the starting tx_num for each cluster in this stage so
        // each worker can derive its TXs' absolute apply-order indices for
        // PRNG seed derivation. tx_num counts up across stages and clusters
        // in the same order the C++ TxSetPhaseFrame::Iterator walked.
        let cluster_starts: Vec<u64> = stage_clusters
            .iter()
            .map(|(begin, end)| {
                let start = next_tx_num;
                next_tx_num += (end - begin) as u64;
                start
            })
            .collect();
        let cluster_starts_ref = &cluster_starts;
        let max_refundable_ref: &Vec<i64> = per_tx_max_refundable_fee;
        // Take ownership of this stage's cluster envelope Vecs so each
        // worker thread gets to consume them by-move. The slot in
        // per_cluster_envelopes is left empty afterwards.
        let mut stage_cluster_envelopes: Vec<Vec<TransactionEnvelope>> =
            Vec::with_capacity(stage_clusters.len());
        for i in 0..stage_clusters.len() {
            stage_cluster_envelopes
                .push(std::mem::take(&mut per_cluster_envelopes[cluster_idx_global + i]));
        }
        cluster_idx_global += stage_clusters.len();
        let cluster_outputs: Vec<
            Result<
                (
                    Vec<crate::SorobanTxApplyResult>,
                    AccumulatedWrites,
                    FastMap<LedgerKey, Vec<u8>>,
                ),
                Box<dyn std::error::Error + Send>,
            >,
        > = std::thread::scope(|s| {
            let handles: Vec<_> = stage_cluster_envelopes
                .into_iter()
                .enumerate()
                .map(|(i, cluster_chunk)| {
                    let start = cluster_starts_ref[i];
                    s.spawn(move || {
                        run_cluster(
                            cluster_chunk,
                            start,
                            prng_seed_bytes,
                            state_ref,
                            cross_stage_ref,
                            classic_prefetch_ref,
                            archived_prefetch_ref,
                            config_max_protocol,
                            ledger_info,
                            rent_fee_ref,
                            module_cache,
                            max_refundable_ref,
                            enable_diagnostics,
                            enable_tx_meta,
                            fee_config_ref,
                            env_sizes_ref,
                        )
                    })
                })
                .collect();
            handles
                .into_iter()
                .map(|h| h.join().expect("soroban-apply cluster worker panicked"))
                .collect()
        });

        // Stage barrier: drain per-cluster outputs in the same order
        // clusters appeared in the txset. Within a cluster TXs are in
        // their original order (run_cluster runs them sequentially).
        for output in cluster_outputs {
            let (tx_results, local_writes, local_host_bytes) =
                output.map_err(|e| -> Box<dyn std::error::Error> { e.to_string().into() })?;
            per_tx.extend(tx_results);
            merge_cluster_into_phase(
                local_writes,
                local_host_bytes,
                &mut accumulated_writes,
                &mut accumulated_host_bytes,
            );
        }
    }

    // End of phase: split per-category ledger_updates and apply to
    // SorobanState in place. Soroban writes are pre-classified
    // init/live/dead (Rust already knew create vs update from
    // `state.get(&k)` at fold time) so the C++ post-pass can route
    // them straight into the bucket batch without a wasCreate map
    // walk over tx_changes.
    let split = apply_phase_writes_to_state(
        state,
        accumulated_writes,
        accumulated_host_bytes,
        config_max_protocol,
        ledger_info.protocol_version,
        ledger_info.cpu_cost_params.as_ref(),
        ledger_info.mem_cost_params.as_ref(),
    )?;

    Ok(SorobanPhaseResult {
        per_tx,
        soroban_init_entry_xdrs: split.soroban_init_entry_xdrs,
        soroban_live_entry_xdrs: split.soroban_live_entry_xdrs,
        soroban_dead_key_xdrs: split.soroban_dead_key_xdrs,
        classic_updates: split.classic_updates,
    })
}

// Drain `accumulated_writes` into both:
//   (a) `SorobanState` — applies each diff via the typed CRUD methods
//       (mirroring the C++ updateState path that C4c removed).
//   (b) the returned Vec<LedgerEntryUpdate> — the C++ post-pass writes
//       these into the live bucket list.
//
// Order matters slightly: TTL writes for newly-created entries must land
// after the entry, otherwise create_ttl can't find its target. We process
// CONTRACT_DATA / CONTRACT_CODE creations + updates first, then TTL writes,
// then deletions.
// Split shape returned by apply_phase_writes_to_state. Mirrors the
// bridge's SorobanPhaseResult layout: Soroban writes are pre-classified
// (init/live/dead) so the C++ post-pass can route them directly to the
// bucket-list batch without a wasCreate map walk over tx_changes.
struct SplitPhaseUpdates {
    // Soroban init/live ship just the encoded entry bytes; the matching
    // LedgerKey is derivable on the C++ side via `LedgerEntryKey(entry)`,
    // which `ltx.createWithoutLoading` / `updateWithoutLoading` invoke
    // internally through `InternalLedgerEntry`.
    soroban_init_entry_xdrs: Vec<RustBuf>,
    soroban_live_entry_xdrs: Vec<RustBuf>,
    // Soroban deletes ship just the encoded LedgerKey — value bytes
    // have no meaning for a delete.
    soroban_dead_key_xdrs: Vec<RustBuf>,
    // Classic side-effects still ship full (key, value-or-empty)
    // because the C++ post-pass mixes load/create/update/erase on
    // ltx and needs the key independently from the entry.
    classic_updates: Vec<LedgerEntryUpdate>,
}

fn apply_phase_writes_to_state(
    state: &mut SorobanState,
    accumulated_writes: AccumulatedWrites,
    mut host_bytes: FastMap<LedgerKey, Vec<u8>>,
    config_max_protocol: u32,
    protocol_version: u32,
    cpu_cost_params: &[u8],
    mem_cost_params: &[u8],
) -> Result<SplitPhaseUpdates, Box<dyn std::error::Error>> {
    // Soroban init/live ship just the encoded entry; C++ derives the
    // LedgerKey from the entry on its side via `InternalLedgerEntry`.
    let mut soroban_init_entry_xdrs: Vec<RustBuf> = Vec::new();
    let mut soroban_live_entry_xdrs: Vec<RustBuf> = Vec::new();
    let mut soroban_dead_key_xdrs: Vec<RustBuf> = Vec::new();
    let mut classic_updates: Vec<LedgerEntryUpdate> = Vec::new();

    // Bucket entries by category for ordered application. Classic entries
    // (Account, Trustline, etc.) are emitted as plain ledger_updates the
    // C++ post-pass routes through LedgerTxn — they don't touch
    // SorobanState. Soroban entries go through the typed CRUD path
    // below, which mutates SorobanState in place AND emits the same
    // ledger_update for bucket writeback.
    let mut data_writes: Vec<(LedgerKey, Option<LedgerEntry>)> = Vec::new();
    let mut code_writes: Vec<(LedgerKey, Option<LedgerEntry>)> = Vec::new();
    let mut ttl_writes: Vec<(LedgerKey, Option<LedgerEntry>)> = Vec::new();
    let mut classic_writes: Vec<(LedgerKey, Option<LedgerEntry>)> = Vec::new();

    for (k, v) in accumulated_writes {
        match &k {
            LedgerKey::ContractData(_) => data_writes.push((k, v)),
            LedgerKey::ContractCode(_) => code_writes.push((k, v)),
            LedgerKey::Ttl(_) => ttl_writes.push((k, v)),
            _ => classic_writes.push((k, v)),
        }
    }

    // Push a Soroban entry's encoded bytes onto a value-only target Vec.
    // C++ derives the LedgerKey from the entry on its side, so init/live
    // updates don't need to ship `key_xdr` over the bridge.
    //
    // The host's `metered_write_xdr` output is reused verbatim when it
    // sits in `host_bytes_cache` (with the `lastModifiedLedgerSeq`
    // patched); otherwise the entry is encoded here. The cache lookup
    // pulls the bytes out by-move so we don't pay for a per-write
    // memcpy.
    fn push_entry_xdr(
        entry_xdrs: &mut Vec<RustBuf>,
        host_bytes_cache: &mut FastMap<LedgerKey, Vec<u8>>,
        key: &LedgerKey,
        entry: &LedgerEntry,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let bytes = match host_bytes_cache.remove(key) {
            Some(cached) => cached,
            None => entry
                .to_xdr(Limits::none())
                .map_err(|e| format!("serialize LedgerEntry: {}", e))?,
        };
        entry_xdrs.push(RustBuf::from(bytes));
        Ok(())
    }

    // Push an already-encoded LedgerKey onto a key-only target Vec
    // (Soroban deletes). Reuses the bytes the caller already encoded
    // for the SHA-256 step instead of re-encoding.
    fn push_dead_key_xdr(key_xdrs: &mut Vec<RustBuf>, key_xdr: Vec<u8>) {
        key_xdrs.push(RustBuf::from(key_xdr));
    }

    // Classic side-effects still need (key, value-or-empty) tuples
    // because the C++ post-pass does load/create/update/erase on ltx
    // and the key is read independently from the entry.
    fn push_classic_update(
        updates: &mut Vec<LedgerEntryUpdate>,
        host_bytes_cache: &mut FastMap<LedgerKey, Vec<u8>>,
        key: &LedgerKey,
        entry: Option<&LedgerEntry>,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let key_xdr = key
            .to_xdr(Limits::none())
            .map_err(|e| format!("serialize LedgerKey: {}", e))?;
        let value_xdr = match entry {
            Some(e) => match host_bytes_cache.remove(key) {
                Some(cached) => cached,
                None => e
                    .to_xdr(Limits::none())
                    .map_err(|e| format!("serialize LedgerEntry: {}", e))?,
            },
            None => Vec::<u8>::new(),
        };
        updates.push(LedgerEntryUpdate {
            key_xdr: RustBuf::from(key_xdr),
            value_xdr: RustBuf::from(value_xdr),
        });
        Ok(())
    }

    // Pass 1+2 drains data_writes / code_writes by-move so each
    // ContractData / ContractCode entry is consumed straight into
    // state.{update,create}_contract_*. Tombstones (None) are deferred
    // to pass 4 — we keep their key hashes on the side so the actual
    // state.delete_*_by_hash runs after the TTL emit-only pass below.
    //
    // The data/code commit paths encode each LedgerKey exactly once
    // here, SHA-256 those bytes to derive the TtlKeyHash, and thread
    // both the encoded bytes (into push_update_with_encoded_key) and
    // the hash (into upsert_*_with_key_hash / contains_*_by_hash) so
    // state.rs doesn't repeat either step.
    let mut data_keys_to_delete: Vec<TtlKeyHash> = Vec::new();
    let mut code_keys_to_delete: Vec<TtlKeyHash> = Vec::new();

    // 1. CONTRACT_DATA creates / updates. Phase-local create+delete
    //    (entry not in base AND accumulated is None) is skipped on
    //    both the state and bucket sides — the bucket never saw the
    //    create, so it doesn't need the delete.
    //
    //    The key_xdr we encode here is used for two things: the
    //    SHA-256-derived TtlKeyHash (for SorobanState lookup) and,
    //    on the delete path, the bridge-side `soroban_dead_key_xdrs`
    //    entry. The init/live path doesn't ship the key over the
    //    bridge — C++ derives it from the entry on its side.
    for (k, v_opt) in data_writes.into_iter() {
        let key_xdr = k
            .to_xdr(Limits::none())
            .map_err(|e| -> Box<dyn std::error::Error> {
                format!("serialize ContractData LedgerKey: {}", e).into()
            })?;
        let key_hash: TtlKeyHash = Sha256::digest(&key_xdr).into();
        match v_opt {
            Some(entry) => {
                // Capture is_new from the upsert before pushing the
                // entry to choose the init/live target. We have to
                // probe `contains_contract_data_by_hash` first because
                // upsert consumes `entry` by-move (and we still need to
                // push it via `push_entry_xdr` afterwards).
                let is_new = !state.contains_contract_data_by_hash(key_hash);
                let target = if is_new {
                    &mut soroban_init_entry_xdrs
                } else {
                    &mut soroban_live_entry_xdrs
                };
                let cached_size = host_bytes.get(&k).map(|b| b.len() as u32);
                push_entry_xdr(target, &mut host_bytes, &k, &entry)?;
                let size = cached_size
                    .unwrap_or_else(|| crate::soroban_apply::common::xdr_serialized_size(&entry));
                state.upsert_contract_data_with_key_hash(entry, size, key_hash);
            }
            None => {
                if state.contains_contract_data_by_hash(key_hash) {
                    // Base-state entry being deleted: emit now (so the
                    // bucket sees it) but delay the actual
                    // state.delete_contract_data until after pass 3 so
                    // pass 3's state.has_ttl probe still finds the
                    // entry's TTL.
                    push_dead_key_xdr(&mut soroban_dead_key_xdrs, key_xdr);
                    data_keys_to_delete.push(key_hash);
                }
            }
        }
    }

    // 2. CONTRACT_CODE creates / updates. Same shape as pass 1; size
    //    feeds the protocol-aware rent-fee compute (xdr_size + parsed
    //    module memory on protocol ≥ 23).
    for (k, v_opt) in code_writes.into_iter() {
        let key_xdr = k
            .to_xdr(Limits::none())
            .map_err(|e| -> Box<dyn std::error::Error> {
                format!("serialize ContractCode LedgerKey: {}", e).into()
            })?;
        let key_hash: TtlKeyHash = Sha256::digest(&key_xdr).into();
        match v_opt {
            Some(entry) => {
                let is_new = !state.contains_contract_code_by_hash(key_hash);
                let target = if is_new {
                    &mut soroban_init_entry_xdrs
                } else {
                    &mut soroban_live_entry_xdrs
                };
                let size_bytes = compute_contract_code_size_for_rent(
                    &entry,
                    config_max_protocol,
                    protocol_version,
                    cpu_cost_params,
                    mem_cost_params,
                );
                push_entry_xdr(target, &mut host_bytes, &k, &entry)?;
                state.upsert_contract_code_with_key_hash(entry, size_bytes, key_hash);
            }
            None => {
                if state.contains_contract_code_by_hash(key_hash) {
                    push_dead_key_xdr(&mut soroban_dead_key_xdrs, key_xdr);
                    code_keys_to_delete.push(key_hash);
                }
            }
        }
    }

    // 3. TTL writes. Must follow the data/code passes so create_ttl
    //    can link to the parent. TTL deletes don't need a separate
    //    delete_ttl call (the TTL lives inside the parent entry and
    //    goes away when pass 4 deletes the parent), but we still emit
    //    the bucket-side tombstone if the parent had a base-state
    //    entry.
    for (k, v_opt) in ttl_writes.into_iter() {
        match v_opt {
            Some(entry) => {
                let exists = state.has_ttl(&k);
                let target = if exists {
                    &mut soroban_live_entry_xdrs
                } else {
                    &mut soroban_init_entry_xdrs
                };
                push_entry_xdr(target, &mut host_bytes, &k, &entry)?;
                if exists {
                    state.update_ttl(entry);
                } else {
                    state.create_ttl(entry);
                }
            }
            None => {
                if state.has_ttl(&k) {
                    // TTL delete: ship the encoded TTL key on the
                    // dead-key wire so the bucket layer erases it.
                    let key_xdr = k.to_xdr(Limits::none()).map_err(|e| {
                        format!("serialize TTL LedgerKey: {}", e)
                    })?;
                    push_dead_key_xdr(&mut soroban_dead_key_xdrs, key_xdr);
                }
            }
        }
    }

    // 4. State deletes — runs after pass 3's TTL emit so the
    //    has_ttl probe sees the parent before it disappears.
    for key_hash in &data_keys_to_delete {
        state.delete_contract_data_by_hash(*key_hash);
    }
    for key_hash in &code_keys_to_delete {
        state.delete_contract_code_by_hash(*key_hash);
    }

    // 5. Classic-side writes — Account / Trustline / etc. emitted by the
    //    Soroban host as side effects of native asset operations. They
    //    don't touch SorobanState; the C++ post-pass routes them through
    //    LedgerTxn for bucket writeback.
    for (k, v_opt) in classic_writes.into_iter() {
        push_classic_update(&mut classic_updates, &mut host_bytes, &k, v_opt.as_ref())?;
    }

    Ok(SplitPhaseUpdates {
        soroban_init_entry_xdrs,
        soroban_live_entry_xdrs,
        soroban_dead_key_xdrs,
        classic_updates,
    })
}

// One cluster's worth of TX execution. Runs the cluster's TXs in order,
// each one reading from `cross_stage_writes` + the cluster-local
// accumulator and pushing its writes into the local accumulator only.
// The orchestrator merges all per-cluster locals into the cross-stage
fn run_cluster(
    cluster: Vec<TransactionEnvelope>,
    starting_tx_num: u64,
    base_prng_seed: &[u8],
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    archived_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    config_max_protocol: u32,
    ledger_info: &CxxLedgerInfo,
    rent_fee_configuration: &CxxRentFeeConfiguration,
    module_cache: &SorobanModuleCache,
    per_tx_max_refundable_fee: &[i64],
    enable_diagnostics: bool,
    enable_tx_meta: bool,
    fee_configuration: &crate::CxxFeeConfiguration,
    per_tx_envelope_size_bytes: &[u32],
) -> Result<
    (
        Vec<crate::SorobanTxApplyResult>,
        AccumulatedWrites,
        FastMap<LedgerKey, Vec<u8>>,
    ),
    Box<dyn std::error::Error + Send>,
> {
    let est_writes: usize = cluster.len().saturating_mul(4);
    let mut local_writes: AccumulatedWrites =
        FastMap::with_capacity_and_hasher(est_writes, FxBuildHasher::default());
    // Per-cluster RO TTL bump buffer. Mirrors the legacy
    // ThreadParallelApplyLedgerState::mRoTTLBumps: TTL bumps coming from
    // RO footprint entries don't propagate inside the cluster (so
    // sibling RO TXs don't see each other's bumps as "prev"), they
    // only get max-merged at cluster-end into local_writes. When a
    // RW TX touches a key, any pending RO bumps for that key get
    // flushed (max-merged into cluster_local_writes) BEFORE the TX
    // runs so the RW writer sees the bumped TTL as its prev.
    // Each entry is `(typed TTL LedgerEntry, encoded XDR bytes)`. Bytes
    // may be empty when the producer doesn't have them on hand (e.g.
    // ExtendFootprintTtl, which constructs the bumped TTL typed-only);
    // in that case the phase-end commit re-encodes from the typed
    // entry. The host-fn path (apply_invoke_host_function) supplies
    // real bytes so the phase-end emit reuses them verbatim.
    let mut ro_ttl_bumps: FastMap<LedgerKey, (LedgerEntry, Vec<u8>)> =
        FastMap::default();
    // Per-cluster cache of host-supplied encoded bytes. Each TX's
    // typed-host call returns `(LedgerEntry, encoded bytes)` pairs; we
    // stash the bytes here keyed by `LedgerKey` so the phase-end
    // ledger_updates emission can skip a re-serialize. Later writes to
    // the same key overwrite earlier bytes (the latest write is what
    // ends up in the bucket).
    let mut host_bytes: FastMap<LedgerKey, Vec<u8>> =
        FastMap::with_capacity_and_hasher(est_writes, FxBuildHasher::default());
    // Per-cluster cache of `Rc<LedgerEntry>` for state-resident
    // entries — see apply_invoke_host_function for the full rationale.
    // Lives at run_cluster scope so a footprint key read by TX 0 can
    // be served as `Rc::clone` for TXs 1..N in the same cluster.
    let mut state_entry_rc_cache: FastMap<TtlKeyHash, std::rc::Rc<LedgerEntry>> =
        FastMap::default();
    let mut tx_results = Vec::with_capacity(cluster.len());
    for (i, tx_envelope) in cluster.into_iter().enumerate() {
        let tx_num = starting_tx_num + i as u64;
        let per_tx_prng_seed = derive_per_tx_prng_seed(base_prng_seed, tx_num);
        let max_refundable_fee = per_tx_max_refundable_fee
            .get(tx_num as usize)
            .copied()
            .unwrap_or(i64::MAX);
        let env_size = per_tx_envelope_size_bytes
            .get(tx_num as usize)
            .copied()
            .unwrap_or(0);
        let result = dispatch_one_tx(
            tx_envelope,
            &per_tx_prng_seed,
            state,
            cross_stage_writes,
            &mut local_writes,
            &mut ro_ttl_bumps,
            &mut host_bytes,
            classic_prefetch,
            archived_prefetch,
            &mut state_entry_rc_cache,
            config_max_protocol,
            ledger_info,
            copy_rent_fee_config(rent_fee_configuration),
            module_cache,
            max_refundable_fee,
            enable_diagnostics,
            enable_tx_meta,
            crate::soroban_apply::common::copy_fee_config(fee_configuration),
            env_size,
        )
        .map_err(|e| -> Box<dyn std::error::Error + Send> {
            // Wrap into a Send error type by stringifying.
            #[derive(Debug)]
            struct SendError(String);
            impl std::fmt::Display for SendError {
                fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                    write!(f, "{}", self.0)
                }
            }
            impl std::error::Error for SendError {}
            Box::new(SendError(e.to_string()))
        })?;
        tx_results.push(result);
    }
    // End of cluster: drain the ro_ttl_bumps buffer into local_writes,
    // taking max live_until_ledger_seq for any TTL key that may already
    // have a (RW-driven) cluster_local entry. The matching encoded
    // bytes flow into host_bytes alongside the winning typed entry so
    // the phase-end commit can hand them straight to the
    // LedgerEntryUpdate without a re-encode. The orchestrator's stage
    // barrier then merges local_writes (and local_host_bytes) into
    // accumulated_writes the same way it does for RW data writes.
    for (ttl_key, (buffered_ttl, bytes)) in ro_ttl_bumps {
        let won = merge_ttl_max(&mut local_writes, ttl_key.clone(), buffered_ttl);
        if won && !bytes.is_empty() {
            host_bytes.insert(ttl_key, bytes);
        }
    }
    Ok((tx_results, local_writes, host_bytes))
}

// Identify the operation in a TransactionEnvelope and dispatch to the
// appropriate per-TX driver, threading the layered state and accumulating
// the resulting writes into the cluster-local layer.
fn dispatch_one_tx(
    tx_envelope: TransactionEnvelope,
    per_tx_prng_seed: &[u8; 32],
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &mut AccumulatedWrites,
    ro_ttl_bumps: &mut FastMap<LedgerKey, (LedgerEntry, Vec<u8>)>,
    host_bytes: &mut FastMap<LedgerKey, Vec<u8>>,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    archived_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    state_entry_rc_cache: &mut FastMap<TtlKeyHash, std::rc::Rc<LedgerEntry>>,
    config_max_protocol: u32,
    ledger_info: &CxxLedgerInfo,
    rent_fee_configuration: CxxRentFeeConfiguration,
    module_cache: &SorobanModuleCache,
    max_refundable_fee: i64,
    enable_diagnostics: bool,
    enable_tx_meta: bool,
    fee_configuration: crate::CxxFeeConfiguration,
    tx_envelope_size_bytes: u32,
) -> Result<crate::SorobanTxApplyResult, Box<dyn std::error::Error>> {
    // Test-only "txINTERNAL_ERROR" memo trigger. Mirrors the legacy
    // BUILD_TESTS hook in TransactionFrame::applyOperations: a TX with
    // this memo is meant to fail with txINTERNAL_ERROR. Skip the host
    // entirely so the TX's writes don't pollute cluster_local_writes /
    // ro_ttl_bumps; signal back via is_internal_error so the C++
    // post-pass can set the right tx result code.
    if has_test_internal_error_memo(&tx_envelope) {
        return Ok(crate::SorobanTxApplyResult {
            success: false,
            is_internal_error: true,
            is_insufficient_refundable_fee: false,
            is_resource_limit_exceeded: false,
            is_entry_archived: false,
            return_value_xdr: RustBuf::from(Vec::<u8>::new()),
            contract_events: Vec::new(),
            diagnostic_events: Vec::new(),
            rent_fee_consumed: 0,
            contract_event_size_bytes: 0,
            tx_changes: Vec::new(),
            hot_archive_restores: Vec::new(),
            live_restores: Vec::new(),
                        success_preimage_hash: RustBuf::from(Vec::<u8>::new()),
                        refundable_fee_increment: 0,
        });
    }

    let (tx_source, operations, soroban_data) = extract_tx_parts_owned(tx_envelope)?;
    if operations.len() != 1 {
        return Err(format!(
            "dispatch_one_tx: expected 1 operation in Soroban TX, got {}",
            operations.len()
        )
        .into());
    }
    let crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::SorobanTransactionData {
        resources,
        ext: soroban_data_ext,
        resource_fee: _,
    } = soroban_data;
    let mut op_iter = operations.into_iter();
    let op = op_iter.next().expect("operations.len() == 1 just validated");
    let source_account: MuxedAccount = op.source_account.unwrap_or(tx_source);

    // For each RW footprint key, flush any pending RO TTL bump for the
    // matching TTL key into cluster_local_writes BEFORE the host runs —
    // mirrors the legacy
    // ThreadParallelApplyLedgerState::flushBufferedRoTTLBumps. This way
    // a RW writer's "prev" TTL view incorporates earlier RO bumps from
    // the same cluster, and the buffer is drained for those keys.
    for k in resources.footprint.read_write.iter() {
        if !matches!(
            k,
            LedgerKey::ContractData(_) | LedgerKey::ContractCode(_)
        ) {
            continue;
        }
        let ttl_key = ttl_lookup_key_for(k);
        if let Some((buffered_ttl, bytes)) = ro_ttl_bumps.remove(&ttl_key) {
            // Take max with whatever cluster_local_writes already has
            // for this TTL (probably nothing — earlier RW writers in
            // the cluster would have produced their own value, in which
            // case the buffered bump only wins if it's higher).
            let won = merge_ttl_max(cluster_local_writes, ttl_key.clone(), buffered_ttl);
            if won && !bytes.is_empty() {
                host_bytes.insert(ttl_key, bytes);
            }
        }
    }

    match op.body {
        OperationBody::InvokeHostFunction(inv_op) => {
            // archivedSorobanEntries (resourceExt v1) — indices into the
            // RW footprint marking entries that should be auto-restored
            // from the hot archive instead of failing with
            // ENTRY_ARCHIVED. Empty for ext == V_0.
            let archived_indices: Vec<u32> = match soroban_data_ext {
                SorobanTransactionDataExt::V0 => Vec::new(),
                SorobanTransactionDataExt::V1(ext) => {
                    ext.archived_soroban_entries.into()
                }
            };
            let crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::InvokeHostFunctionOp {
                host_function,
                auth,
            } = inv_op;
            // auth is a VecM<SorobanAuthorizationEntry, _>; convert to
            // Vec<...> by-move (From<VecM<T,_>> for Vec<T> exists).
            let auth_entries: Vec<_> = auth.into();
            apply_invoke_host_function(
                state,
                cross_stage_writes,
                cluster_local_writes,
                ro_ttl_bumps,
                host_bytes,
                classic_prefetch,
                archived_prefetch,
                state_entry_rc_cache,
                config_max_protocol,
                ledger_info,
                rent_fee_configuration,
                module_cache,
                source_account,
                host_function,
                auth_entries,
                &resources,
                &archived_indices,
                per_tx_prng_seed,
                max_refundable_fee,
                enable_diagnostics,
                enable_tx_meta,
                fee_configuration,
                tx_envelope_size_bytes,
            )
        }
        OperationBody::ExtendFootprintTtl(ext_op) => {
            apply_extend_footprint_ttl(
                state,
                cross_stage_writes,
                cluster_local_writes,
                ro_ttl_bumps,
                classic_prefetch,
                config_max_protocol,
                ledger_info,
                rent_fee_configuration,
                &ext_op,
                &resources,
                max_refundable_fee,
            )
        }
        OperationBody::RestoreFootprint(_) => apply_restore_footprint(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            archived_prefetch,
            config_max_protocol,
            ledger_info,
            rent_fee_configuration,
            &resources,
            max_refundable_fee,
        ),
        other => Err(format!(
            "dispatch_one_tx: non-Soroban operation type {:?} in Soroban phase",
            std::mem::discriminant(&other)
        )
        .into()),
    }
}

// Stage-barrier merge: fold one cluster's writes / host-byte cache into
// the phase-level accumulators. TTL writes are max-merged on
// live_until_ledger_seq so a later cluster's lower bump doesn't overwrite
// an earlier cluster's higher bump (mirroring the legacy
// GlobalParallelApplyLedgerState::commitChangeFromThread behaviour). Non-
// TTL writes overwrite — RW-conflict detection at txset construction
// keeps two clusters from writing the same data/code key in the same
// stage. Host-supplied bytes flow alongside the winning typed value;
// when the incoming write doesn't win, the kept entry's existing bytes
// already match the chosen typed value (or, if neither cluster supplied
// bytes, apply_phase_writes_to_state will serialize on demand).
fn merge_cluster_into_phase(
    local_writes: AccumulatedWrites,
    mut local_host_bytes: FastMap<LedgerKey, Vec<u8>>,
    accumulated_writes: &mut AccumulatedWrites,
    accumulated_host_bytes: &mut FastMap<LedgerKey, Vec<u8>>,
) {
    for (k, v) in local_writes {
        let incoming_wins = if matches!(k, LedgerKey::Ttl(_)) {
            let incoming = v.as_ref().and_then(ttl_live_until_of);
            let existing = ttl_live_until_in_writes(accumulated_writes, &k);
            !matches!((existing, incoming), (Some(e), Some(i)) if e >= i)
        } else {
            true
        };
        if !incoming_wins {
            continue;
        }
        if let Some(bytes) = local_host_bytes.remove(&k) {
            accumulated_host_bytes.insert(k.clone(), bytes);
        } else {
            // No incoming bytes — invalidate any stale cached bytes
            // for this key so apply_phase_writes_to_state re-serializes
            // from the typed entry.
            accumulated_host_bytes.remove(&k);
        }
        accumulated_writes.insert(k, v);
    }
}

// Decode `soroban_envelopes` into a parallel Vec<TransactionEnvelope>.
// Each TransactionEnvelope decode is independent, so the work fans out
// across up to MAX_WORKERS std::thread::scope workers, writing into the
// pre-sized output Vec at disjoint indices.
fn decode_envelopes_parallel(
    soroban_envelopes: &[CxxBuf],
) -> Result<Vec<TransactionEnvelope>, Box<dyn std::error::Error>> {
    let total_txs = soroban_envelopes.len();
    if total_txs == 0 {
        return Ok(Vec::new());
    }
    const MAX_WORKERS: usize = 8;
    let workers = std::cmp::min(
        MAX_WORKERS,
        std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(1),
    );
    let workers = std::cmp::min(workers, total_txs);
    if workers <= 1 {
        return soroban_envelopes
            .iter()
            .map(|b| {
                TransactionEnvelope::from_xdr(b.as_ref(), Limits::none())
                    .map_err(|e| -> Box<dyn std::error::Error> { e.into() })
            })
            .collect();
    }
    let mut envs: Vec<Option<TransactionEnvelope>> = (0..total_txs).map(|_| None).collect();
    let envs_ptr_usize = envs.as_mut_ptr() as usize;
    std::thread::scope(|s| -> Result<(), Box<dyn std::error::Error>> {
        let base = total_txs / workers;
        let rem = total_txs % workers;
        let mut handles = Vec::with_capacity(workers);
        let mut cursor = 0usize;
        for w in 0..workers {
            let chunk = base + if w < rem { 1 } else { 0 };
            let begin = cursor;
            let end = begin + chunk;
            cursor = end;
            let envs_ptr_usize = envs_ptr_usize;
            handles.push(s.spawn(move || -> Result<(), String> {
                for i in begin..end {
                    let env = TransactionEnvelope::from_xdr(
                        soroban_envelopes[i].as_ref(),
                        Limits::none(),
                    )
                    .map_err(|e| e.to_string())?;
                    // Safety: each worker writes to a disjoint index
                    // range; the indices are pre-partitioned and the
                    // Vec is pre-sized so there's no aliased write or
                    // Vec growth.
                    unsafe {
                        let p = envs_ptr_usize as *mut Option<TransactionEnvelope>;
                        std::ptr::write(p.add(i), Some(env));
                    }
                }
                Ok(())
            }));
        }
        for h in handles {
            h.join()
                .expect("envelope decode worker panicked")
                .map_err(|e| -> Box<dyn std::error::Error> { e.into() })?;
        }
        Ok(())
    })?;
    Ok(envs.into_iter().map(|o| o.unwrap()).collect())
}

// Reconstruct the stage / cluster index structure from the flat counts.
// Returns `stages_idx` where `stages_idx[i]` is the per-cluster envelope
// range slice for stage i; each cluster slice is `(start, end)` into the
// flat envelope vec.
fn build_stage_cluster_indices(
    cluster_sizes: &[u32],
    stage_cluster_counts: &[u32],
    total_txs: usize,
) -> Result<Vec<Vec<(usize, usize)>>, Box<dyn std::error::Error>> {
    let mut stages_idx: Vec<Vec<(usize, usize)>> =
        Vec::with_capacity(stage_cluster_counts.len());
    let mut cluster_iter = cluster_sizes.iter();
    let mut env_offset: usize = 0;
    for &nclusters in stage_cluster_counts.iter() {
        let mut clusters = Vec::with_capacity(nclusters as usize);
        for _ in 0..nclusters {
            let sz = *cluster_iter.next().ok_or_else(
                || -> Box<dyn std::error::Error> {
                    "apply_soroban_phase: cluster_sizes / stage_cluster_counts out of sync".into()
                },
            )? as usize;
            clusters.push((env_offset, env_offset + sz));
            env_offset += sz;
        }
        stages_idx.push(clusters);
    }
    if env_offset != total_txs {
        return Err(
            "apply_soroban_phase: envelope count doesn't match cluster size sum".into(),
        );
    }
    Ok(stages_idx)
}

// InvokeHostFunction per-TX wiring. Builds the host inputs from layered
// state, calls invoke_host_function_old_env, and folds modified entries
// into the accumulator.
