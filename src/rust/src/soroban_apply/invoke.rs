// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! InvokeHostFunction op driver. Wraps the per-protocol soroban-env-host
//! `invoke_host_function_*` entry points with the typed-input / typed-output
//! plumbing the apply phase needs, and applies the host's results to the
//! per-cluster accumulator.

use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::{
    AccountId, ContractDataDurability, ContractEvent, ContractEventBody, ContractEventType,
    ContractEventV0, DiagnosticEvent, ExtensionPoint, Hash, HostFunction,
    InvokeHostFunctionOp, LedgerEntry, LedgerEntryData, LedgerEntryExt, LedgerKey, Limits,
    MuxedAccount, ReadXdr, ScSymbol, ScVal, SorobanAuthorizationEntry, SorobanResources,
    TtlEntry, VecM, WriteXdr,
};

use super::common::{
    build_tx_delta, build_tx_delta_with_cached_new, bytes_to_cxx_buf, layered_get,
    ledger_entry_key, make_tx_failure_result, muxed_to_account_id_owned,
    patch_last_modified_seq, ttl_key_hash_for, ttl_lookup_key_for, xdr_serialized_size,
    xdr_to_cxx_buf, xdr_to_vec, AccumulatedWrites, FastMap, FastSet, SorobanTxFailure, TtlKeyHash,
};
use super::state::SorobanState;
use crate::{
    CxxBuf, CxxLedgerInfo, CxxRentFeeConfiguration, LedgerEntryUpdate, RustBuf,
    SorobanModuleCache,
};

#[derive(Default)]
struct InvokeMetrics {
    read_entry: u32,
    write_entry: u32,
    ledger_read_byte: u32,
    ledger_write_byte: u32,
    read_key_byte: u32,
    write_key_byte: u32,
    read_data_byte: u32,
    write_data_byte: u32,
    read_code_byte: u32,
    write_code_byte: u32,
    emit_event: u32,
    emit_event_byte: u32,
    cpu_insn: u64,
    mem_byte: u64,
    invoke_time_nsecs: u64,
    max_rw_key_byte: u32,
    max_rw_data_byte: u32,
    max_rw_code_byte: u32,
    max_emit_event_byte: u32,
}

// Build a "core_metrics" diagnostic event matching the legacy
// InvokeHostFunctionOpFrame::metricsEvent shape: topics=[Symbol(
// "core_metrics"), Symbol(<topic>)], data=U64(value).
fn make_metric_event(success: bool, topic: &str, value: u64) -> DiagnosticEvent {
    let topics: VecM<ScVal> = vec![
        ScVal::Symbol(ScSymbol(
            "core_metrics".try_into().unwrap_or_default(),
        )),
        ScVal::Symbol(ScSymbol(topic.try_into().unwrap_or_default())),
    ]
    .try_into()
    .unwrap_or_default();
    DiagnosticEvent {
        in_successful_contract_call: success,
        event: ContractEvent {
            ext: ExtensionPoint::V0,
            contract_id: None,
            type_: ContractEventType::Diagnostic,
            body: ContractEventBody::V0(ContractEventV0 {
                topics,
                data: ScVal::U64(value),
            }),
        },
    }
}

// Serialize the 19 core_metrics events the legacy
// InvokeHostFunctionOpFrame::publishMetricsEvents emitted in order, and
// append the resulting XDR bytes to `events`. Order matters: tests
// check specific event indices.
fn append_core_metrics_events(events: &mut Vec<Vec<u8>>, success: bool, m: &InvokeMetrics) {
    let metrics: [(&str, u64); 19] = [
        ("read_entry", m.read_entry as u64),
        ("write_entry", m.write_entry as u64),
        ("ledger_read_byte", m.ledger_read_byte as u64),
        ("ledger_write_byte", m.ledger_write_byte as u64),
        ("read_key_byte", m.read_key_byte as u64),
        ("write_key_byte", m.write_key_byte as u64),
        ("read_data_byte", m.read_data_byte as u64),
        ("write_data_byte", m.write_data_byte as u64),
        ("read_code_byte", m.read_code_byte as u64),
        ("write_code_byte", m.write_code_byte as u64),
        ("emit_event", m.emit_event as u64),
        ("emit_event_byte", m.emit_event_byte as u64),
        ("cpu_insn", m.cpu_insn),
        ("mem_byte", m.mem_byte),
        ("invoke_time_nsecs", m.invoke_time_nsecs),
        ("max_rw_key_byte", m.max_rw_key_byte as u64),
        ("max_rw_data_byte", m.max_rw_data_byte as u64),
        ("max_rw_code_byte", m.max_rw_code_byte as u64),
        ("max_emit_event_byte", m.max_emit_event_byte as u64),
    ];
    for (topic, value) in metrics.iter() {
        let de = make_metric_event(success, topic, *value);
        if let Ok(bytes) = de.to_xdr(Limits::none()) {
            events.push(bytes);
        }
    }
}

// Result of one InvokeHostFunction host invocation, in the typed/bytes hybrid
// shape the per-TX driver needs to fold the host's outputs back into the
// cluster-local accumulator.
struct InvokeHostFunctionTypedResult {
    success: bool,
    is_internal_error: bool,
    cpu_insns: u64,
    mem_bytes: u64,
    time_nsecs: u64,

    // Populated only on success.
    return_value_xdr: Vec<u8>,
    modified_ledger_entries: Vec<(LedgerEntry, Vec<u8>)>,
    contract_events_xdr: Vec<Vec<u8>>,
    rent_fee: i64,

    // Populated regardless of success/failure.
    diagnostic_events_xdr: Vec<Vec<u8>>,
}

// Run one InvokeHostFunction operation against an old (p21..p26) env via the
// existing byte-based host bridge.
//
// Inputs are mostly typed (latest stellar-xdr) — they get serialized once
// per TX into the byte form the host expects. Outputs that the orchestrator
// or the post-pass needs in typed form (modified ledger entries, return
// value) are parsed back here; events stay as bytes since they flow into
// ledger meta as bytes anyway.
//
// Takes the serialized footprint by-move (data entries → CxxBuf with the
// wrapping LedgerEntry, TTL entries → CxxBuf with the inner TtlEntry,
// empty CxxBuf for missing TTL slots) so we hand the same Vec<CxxBuf>
// straight to the bridge — no per-entry rebuild and no double-encode.
//
// `restored_rw_entry_indices` is the same Vec<u32> the existing FFI
// signature expects — indices into the RW footprint of entries that were
// restored from the hot archive earlier in the TX bundle.
fn invoke_host_function_old_env_serialized(
    config_max_protocol: u32,
    enable_diagnostics: bool,
    instruction_limit: u32,
    host_function: &HostFunction,
    resources: &SorobanResources,
    restored_rw_entry_indices: &[u32],
    source_account: &MuxedAccount,
    auth_entries: &[SorobanAuthorizationEntry],
    ledger_info: &CxxLedgerInfo,
    ledger_entry_bufs: Vec<CxxBuf>,
    ttl_entry_bufs: Vec<CxxBuf>,
    base_prng_seed: &[u8],
    rent_fee_configuration: CxxRentFeeConfiguration,
    module_cache: &SorobanModuleCache,
) -> Result<InvokeHostFunctionTypedResult, Box<dyn std::error::Error>> {
    assert_eq!(
        ledger_entry_bufs.len(),
        ttl_entry_bufs.len(),
        "invoke_host_function_old_env_serialized: ledger / TTL footprint length mismatch"
    );

    let hf_buf = xdr_to_cxx_buf(host_function);
    let resources_buf = xdr_to_cxx_buf(resources);
    let source_account_buf = xdr_to_cxx_buf(source_account);
    let prng_buf = bytes_to_cxx_buf(base_prng_seed);

    let auth_bufs: Vec<CxxBuf> =
        auth_entries.iter().map(xdr_to_cxx_buf).collect();

    let restored_rw_entry_indices_vec: Vec<u32> = restored_rw_entry_indices.to_vec();
    let output = crate::soroban_invoke::invoke_host_function(
        config_max_protocol,
        enable_diagnostics,
        instruction_limit,
        &hf_buf,
        resources_buf,
        &restored_rw_entry_indices_vec,
        &source_account_buf,
        &auth_bufs,
        ledger_info,
        &ledger_entry_bufs,
        &ttl_entry_bufs,
        &prng_buf,
        rent_fee_configuration,
        module_cache,
    )?;

    // Bytes path: each modified entry comes back encoded; parse once into
    // typed form and keep the bytes alongside so apply_phase_writes_to_state
    // can emit them as LedgerEntryUpdate without re-encoding.
    let mut modified_ledger_entries: Vec<(LedgerEntry, Vec<u8>)> =
        Vec::with_capacity(output.modified_ledger_entries.len());
    for buf in output.modified_ledger_entries.into_iter() {
        let entry = LedgerEntry::from_xdr(&buf.data, Limits::none())?;
        modified_ledger_entries.push((entry, buf.data));
    }

    // Keep the host's ScVal return-value bytes as-is; the C++ post-pass
    // hashes them into the InvokeHostFunctionResult success preimage,
    // so the typed decode + re-encode roundtrip was pure waste.
    let return_value_xdr = if output.success {
        output.result_value.data
    } else {
        Vec::<u8>::new()
    };

    // The bridge handed us already-owned Vec<u8> per event (via RustBuf);
    // move them out instead of copying byte-by-byte.
    let contract_events_xdr: Vec<Vec<u8>> =
        output.contract_events.into_iter().map(|b| b.data).collect();
    let diagnostic_events_xdr: Vec<Vec<u8>> = output
        .diagnostic_events
        .into_iter()
        .map(|b| b.data)
        .collect();

    Ok(InvokeHostFunctionTypedResult {
        success: output.success,
        is_internal_error: output.is_internal_error,
        cpu_insns: output.cpu_insns,
        mem_bytes: output.mem_bytes,
        time_nsecs: output.time_nsecs,
        return_value_xdr,
        modified_ledger_entries,
        contract_events_xdr,
        rent_fee: output.rent_fee,
        diagnostic_events_xdr,
    })
}

// Run one InvokeHostFunction operation against the V_26 typed-input /
// typed-output host bridge. Bypasses per-input XDR encode / decode of
// the footprint, host function and source account.
fn invoke_host_function_typed_curr(
    enable_diagnostics: bool,
    instruction_limit: u32,
    host_function: HostFunction,
    resources: SorobanResources,
    restored_rw_entry_indices: &[u32],
    source_account: AccountId,
    auth_entries: Vec<SorobanAuthorizationEntry>,
    ledger_info: &CxxLedgerInfo,
    ledger_entries: Vec<(std::rc::Rc<LedgerEntry>, Option<TtlEntry>, u32)>,
    base_prng_seed: [u8; 32],
    rent_fee_configuration: CxxRentFeeConfiguration,
    module_cache: &SorobanModuleCache,
) -> Result<InvokeHostFunctionTypedResult, Box<dyn std::error::Error>> {
    let output = crate::soroban_invoke::invoke_host_function_typed(
        enable_diagnostics,
        instruction_limit,
        host_function,
        resources,
        restored_rw_entry_indices,
        source_account,
        auth_entries,
        ledger_info,
        ledger_entries,
        base_prng_seed,
        rent_fee_configuration,
        module_cache,
    )?;

    // Typed-output path: the host returns each modified entry as a
    // (typed, encoded-bytes) pair. Keep both — the typed view feeds
    // layered-state updates and host-output processing while the
    // bytes flow through to apply_phase_writes_to_state /
    // build_tx_delta to skip a re-encode. The entry's
    // last_modified_ledger_seq still has the host-side value (the
    // host doesn't bump it); apply_invoke_host_function bumps the
    // typed copy AND patches the first 4 bytes of the encoded copy
    // before emitting it.
    let modified_ledger_entries: Vec<(LedgerEntry, Vec<u8>)> = output
        .modified_ledger_entries
        .into_iter()
        .map(|(entry, encoded)| (entry, encoded.data))
        .collect();

    let return_value_xdr = if output.success {
        output.result_value.data
    } else {
        Vec::<u8>::new()
    };

    let contract_events_xdr: Vec<Vec<u8>> =
        output.contract_events.into_iter().map(|b| b.data).collect();
    let diagnostic_events_xdr: Vec<Vec<u8>> = output
        .diagnostic_events
        .into_iter()
        .map(|b| b.data)
        .collect();

    Ok(InvokeHostFunctionTypedResult {
        success: output.success,
        is_internal_error: output.is_internal_error,
        cpu_insns: output.cpu_insns,
        mem_bytes: output.mem_bytes,
        time_nsecs: output.time_nsecs,
        return_value_xdr,
        modified_ledger_entries,
        contract_events_xdr,
        rent_fee: output.rent_fee,
        diagnostic_events_xdr,
    })
}

// Per-TX driver for the InvokeHostFunction op: walks the footprint, resolves
// each slot through the layered state, calls the host, then folds the host's
// modified entries / events / fees back into the cluster-local accumulator.
// Mirrors the legacy C++ InvokeHostFunctionOpFrame::doApply +
// parallelApply flow.
pub(super) fn apply_invoke_host_function(
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
    // Owned values consumed from the parsed envelope at dispatch time —
    // moved straight into the host call rather than cloned from
    // `&InvokeHostFunctionOp` / `&MuxedAccount` borrows. resources stays
    // borrowed because the post-host delete-detection loop reads
    // resources.footprint AFTER the host has consumed its own clone.
    source_account: MuxedAccount,
    host_function: HostFunction,
    auth_entries: Vec<SorobanAuthorizationEntry>,
    resources: &SorobanResources,
    archived_rw_indices: &[u32],
    per_tx_prng_seed: &[u8; 32],
    max_refundable_fee: i64,
    enable_diagnostics: bool,
    enable_tx_meta: bool,
    fee_configuration: crate::CxxFeeConfiguration,
    tx_envelope_size_bytes: u32,
) -> Result<crate::SorobanTxApplyResult, Box<dyn std::error::Error>> {
    // Walk the footprint once and gather already-loaded entries into a
    // typed Vec<(LedgerEntry, Option<TtlEntry>)>:
    //   * `layered_get` returns a `Cow<LedgerEntry>` so state-sourced
    //     entries are borrowed; we only clone at the boundary into the
    //     owned Vec the host needs.
    //   * On the typed (V_26+ soroban_curr) path the host's e2e_invoke
    //     consumes the typed entries via `metered_clone`, skipping the
    //     per-input XDR encode/decode entirely.
    // For older protocols (V_21..V_25), we still need the bytes form,
    // so we serialize once at the FFI boundary below.
    let footprint = &resources.footprint;
    let ro_count = footprint.read_only.len();
    let footprint_keys: Vec<&LedgerKey> = footprint
        .read_only
        .iter()
        .chain(footprint.read_write.iter())
        .collect();
    let mut typed_ledger_entries: Vec<(std::rc::Rc<LedgerEntry>, Option<TtlEntry>, u32)> =
        Vec::with_capacity(footprint_keys.len());
    // Auto-restore TTL: V_23+ host auto-restores marked entries to the
    // network's min_persistent_ttl. liveUntil = ledgerSeq +
    // min_persistent_ttl - 1, mirroring the C++ restoredLiveUntilLedger.
    let restored_live_until = ledger_info
        .sequence_number
        .saturating_add(ledger_info.min_persistent_entry_ttl)
        .saturating_sub(1);
    // Track auto-restored RW entries: their data + freshly-built TTL flow
    // back into cluster_local_writes (for next-stage observation) and
    // hot_archive_restores (for C++ post-pass meta + bucket-side
    // markRestoredFromHotArchive).
    let archived_rw_set: std::collections::HashSet<u32> =
        archived_rw_indices.iter().copied().collect();
    let mut auto_restore_rw_indices: Vec<u32> = Vec::new();
    // Data-side restore records share the typed entry's Rc with the
    // host-input vec so we only allocate one LedgerEntry per restored
    // slot. TTL records are freshly constructed per slot (key_hash +
    // restored_live_until) and aren't shared.
    let mut auto_restored_data_writes: Vec<(LedgerKey, std::rc::Rc<LedgerEntry>)> =
        Vec::new();
    let mut auto_restored_ttl_writes: Vec<(LedgerKey, LedgerEntry)> =
        Vec::new();
    // Live-bucket auto-restores: entries that were already in live
    // state (with expired TTL) and are being marked restored. The
    // C++ post-pass uses these via processOpLedgerEntryChanges to
    // reclassify STATE+UPDATED → RESTORED in meta.
    let mut auto_restored_live_data: Vec<(LedgerKey, std::rc::Rc<LedgerEntry>)> =
        Vec::new();
    let mut auto_restored_live_ttl: Vec<(LedgerKey, LedgerEntry)> =
        Vec::new();
    // Disk-read byte metering. Mirrors the legacy
    // InvokeHostFunctionOpFrame::meterDiskReadResource path: for every
    // non-Soroban entry loaded (account etc., currently the only ones
    // counted; auto-restored entries land in C9b-ii) sum its XDR size
    // and check the running total against resources.disk_read_bytes.
    // The host's e2e_invoke does NOT enforce this cap — the C++ shim
    // owned it before, so the new orchestrator has to do it instead.
    let mut disk_read_bytes_consumed: u32 = 0;
    let disk_read_bytes_limit: u32 = resources.disk_read_bytes;
    let ledger_seq = ledger_info.sequence_number;
    for (k_idx, k) in footprint_keys.iter().enumerate() {
        // RW index for this footprint key (Some only if the slot is in
        // the read_write portion of the footprint). archivedSorobanEntries
        // indices are relative to the read_write section.
        let rw_idx: Option<u32> = if k_idx >= ro_count {
            u32::try_from(k_idx - ro_count).ok()
        } else {
            None
        };
        let is_auto_restore_target = rw_idx
            .map(|i| archived_rw_set.contains(&i))
            .unwrap_or(false);

        // If the key has an explicit tombstone in cluster_local_writes
        // or cross_stage_writes, an earlier TX in this ledger already
        // deleted it (and, if it was archived, already recorded the
        // markRestored side-effect on its own behalf). For this TX,
        // the slot is "absent" — the host should not see the old
        // value, and we must not duplicate the hot-archive restore
        // bookkeeping. Skip the slot entirely; the host will treat
        // it as missing and a put-* call will create a fresh entry.
        let is_tombstoned_in_phase = matches!(
            k,
            LedgerKey::ContractData(_) | LedgerKey::ContractCode(_)
        ) && (cluster_local_writes
            .get(*k)
            .map(|s| s.is_none())
            .unwrap_or(false)
            || cross_stage_writes
                .get(*k)
                .map(|s| s.is_none())
                .unwrap_or(false));
        if is_tombstoned_in_phase {
            continue;
        }

        // Auto-restore branch: this slot's RW index is in
        // archivedSorobanEntries. The entry may be:
        //  (a) Already live (for example: contract instance / code that
        //      the test or legitimate flow extended earlier — index
        //      points at it but no actual restore work is needed). The
        //      host still sees it via `restored_rw_entry_indices`, so
        //      rent accounting treats it as freshly restored.
        //  (b) In live state but with an expired TTL (live-bucket
        //      restore). Host gets the live value + a fresh TTL.
        //  (c) Evicted to the hot archive. Host gets the archived
        //      value + a fresh TTL; we record the restoration so the
        //      C++ post-pass can call markRestoredFromHotArchive.
        // If neither layered_get nor archived_prefetch finds the
        // entry, the footprint hint is malformed — fail.
        if is_auto_restore_target {
            let from_state = layered_get(
                state,
                cross_stage_writes,
                cluster_local_writes,
                classic_prefetch,
                k,
            );
            let entry_and_archived: Option<(LedgerEntry, bool)> = match from_state {
                Some(cow) => Some((cow.into_owned(), false)),
                None => archived_prefetch.get(*k).cloned().map(|e| (e, true)),
            };
            let (entry_value, was_archived) = match entry_and_archived {
                Some(v) => v,
                None => {
                    // Hint says auto-restore but neither live state
                    // nor hot archive has the entry. This happens when
                    // a sibling TX in the same ledger / stage already
                    // deleted the entry — the auto-restore index is
                    // still set on this TX's spec, but there's nothing
                    // to actually restore. Treat the slot as a fresh
                    // create (skip the auto-restore branch entirely
                    // and fall through to the regular flow).
                    let entry_cow = match layered_get(
                        state,
                        cross_stage_writes,
                        cluster_local_writes,
                        classic_prefetch,
                        k,
                    ) {
                        Some(e) => e,
                        None => continue,
                    };
                    let ttl_entry: Option<TtlEntry> = match k {
                        LedgerKey::ContractData(_)
                        | LedgerKey::ContractCode(_) => {
                            let ttl_key = ttl_lookup_key_for(k);
                            match layered_get(
                                state,
                                cross_stage_writes,
                                cluster_local_writes,
                                classic_prefetch,
                                &ttl_key,
                            ) {
                                Some(ttl_cow) => match &ttl_cow.data {
                                    LedgerEntryData::Ttl(t) => Some(t.clone()),
                                    _ => None,
                                },
                                None => None,
                            }
                        }
                        _ => None,
                    };
                    let entry_size_for_typed = xdr_serialized_size(&entry_cow);
                    typed_ledger_entries.push((
                        std::rc::Rc::new(entry_cow.into_owned()),
                        ttl_entry,
                        entry_size_for_typed,
                    ));
                    continue;
                }
            };
            // Build the fresh TTL entry the host expects + the
            // accumulator + post-pass paths.
            let key_hash = ttl_key_hash_for(k);
            let new_ttl = LedgerEntry {
                last_modified_ledger_seq: ledger_seq,
                data: LedgerEntryData::Ttl(TtlEntry {
                    key_hash: Hash(key_hash),
                    live_until_ledger_seq: restored_live_until,
                }),
                ext: LedgerEntryExt::V0,
            };
            let ttl_inner = TtlEntry {
                key_hash: Hash(key_hash),
                live_until_ledger_seq: restored_live_until,
            };
            auto_restore_rw_indices.push(rw_idx.unwrap());
            // Track the restore-source entry for the C++ post-pass.
            // Hot-archive entries flow into hot_archive_restores (so
            // markRestoredFromHotArchive removes them from the hot
            // archive on commit). Already-live entries with expired
            // TTL flow into live_restores (so
            // processOpLedgerEntryChanges reclassifies STATE+UPDATED
            // → RESTORED for the data + TTL meta). Already-live
            // entries with valid TTL (i.e. nothing to actually
            // restore — the auto-restore index is a rent-accounting
            // hint only) are NOT recorded as restores, otherwise the
            // ArchivedStateConsistency invariant fires (the entry
            // wasn't expired so it wasn't really restored).
            // Disk-read cost: count restored entry bytes against the
            // tx's diskReadBytes cap. Mirrors the legacy
            // RestoreFootprintOpFrame::meterDiskReadResource path.
            let entry_value_size = xdr_serialized_size(&entry_value);
            disk_read_bytes_consumed =
                disk_read_bytes_consumed.saturating_add(entry_value_size);
            // Wrap the entry in Rc once: the auto_restored_* bookkeeping
            // shares the same allocation with the typed_ledger_entries
            // input the host consumes via Rc::clone.
            let entry_rc = std::rc::Rc::new(entry_value);
            if was_archived {
                auto_restored_data_writes
                    .push(((*k).clone(), std::rc::Rc::clone(&entry_rc)));
                auto_restored_ttl_writes.push((ttl_lookup_key_for(k), new_ttl));
            } else {
                // Look up the existing TTL to decide live-bucket-restore
                // vs no-op: if TTL exists in state and is live (>=
                // current ledger), this is a no-op marker.
                let ttl_key = ttl_lookup_key_for(k);
                let existing_ttl_live = match layered_get(
                    state,
                    cross_stage_writes,
                    cluster_local_writes,
                    classic_prefetch,
                    &ttl_key,
                ) {
                    Some(ttl_cow) => match &ttl_cow.data {
                        LedgerEntryData::Ttl(t) => {
                            t.live_until_ledger_seq >= ledger_seq
                        }
                        _ => false,
                    },
                    None => false,
                };
                if !existing_ttl_live {
                    auto_restored_live_data
                        .push(((*k).clone(), std::rc::Rc::clone(&entry_rc)));
                    auto_restored_live_ttl.push((ttl_key, new_ttl));
                }
            }
            typed_ledger_entries.push((
                entry_rc,
                Some(ttl_inner),
                entry_value_size,
            ));
            continue;
        }

        let entry_cow = match layered_get(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            k,
        ) {
            Some(e) => e,
            None => {
                // Entry is not in the live state. For persistent
                // CONTRACT_DATA / CONTRACT_CODE keys, this can mean
                // either (a) the entry is archived in the hot archive
                // and the TX should fail with ENTRY_ARCHIVED, or (b)
                // the entry doesn't exist at all (legitimate
                // create-via-write). Differentiate by consulting the
                // hot-archive prefetch. Temporary entries are never
                // archived — they're just absent.
                // archived_prefetch is a static snapshot built before
                // the phase started. If a sibling TX in this phase
                // already auto-restored + deleted the key (visible as
                // an explicit None in cluster_local / cross_stage),
                // treat it as a phase-deleted slot, NOT an archived
                // one — the host will see the slot as absent and the
                // TX shouldn't fail with ENTRY_ARCHIVED.
                let phase_deleted = cluster_local_writes
                    .get(*k)
                    .map(|s| s.is_none())
                    .unwrap_or(false)
                    || cross_stage_writes
                        .get(*k)
                        .map(|s| s.is_none())
                        .unwrap_or(false);
                let is_archived = !phase_deleted
                    && match k {
                        LedgerKey::ContractData(d)
                            if d.durability
                                == ContractDataDurability::Persistent =>
                        {
                            archived_prefetch.contains_key(*k)
                        }
                        LedgerKey::ContractCode(_) => {
                            archived_prefetch.contains_key(*k)
                        }
                        _ => false,
                    };
                if is_archived {
                    return Ok(make_tx_failure_result(
                        SorobanTxFailure::EntryArchived,
                        Vec::new(),
                    ));
                }
                continue;
            }
        };
        // For Soroban data/code keys, derive the TtlKeyHash once and
        // reuse it across (a) the TTL shadow probe in cluster_local /
        // cross_stage, (b) the state TTL lookup, and (c) the
        // state_entry_rc_cache. Classic keys get None — the cache
        // doesn't fire for them and they have no TTL.
        let key_hash_for_soroban: Option<TtlKeyHash> = match k {
            LedgerKey::ContractData(_) | LedgerKey::ContractCode(_) => {
                Some(ttl_key_hash_for(k))
            }
            _ => None,
        };
        let ttl_entry: Option<TtlEntry> = match k {
            LedgerKey::ContractData(_) | LedgerKey::ContractCode(_) => {
                let key_hash = key_hash_for_soroban.unwrap();
                let ttl_key = LedgerKey::Ttl(crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::LedgerKeyTtl {
                    key_hash: Hash(key_hash),
                });
                if let Some(slot) = cluster_local_writes.get(&ttl_key) {
                    match slot.as_ref().map(|le| &le.data) {
                        Some(LedgerEntryData::Ttl(t)) => Some(t.clone()),
                        _ => None,
                    }
                } else if let Some(slot) = cross_stage_writes.get(&ttl_key) {
                    match slot.as_ref().map(|le| &le.data) {
                        Some(LedgerEntryData::Ttl(t)) => Some(t.clone()),
                        _ => None,
                    }
                } else {
                    // Direct state access — skips
                    // `state.get_ttl_owned`'s LedgerEntry synthesis.
                    state.get_ttl_entry_by_hash(key_hash, k)
                }
            }
            _ => None,
        };
        // Archival check (mirrors legacy doApply pre-host walk). For each
        // Soroban footprint key, look at the TTL we just resolved:
        //   * TTL exists and is *expired*:
        //     - persistent entry → ENTRY_ARCHIVED (the host can't run, the
        //       TX fails before the host invocation).
        //     - temporary entry → skip the slot entirely (treat as if the
        //       key did not exist — the host will report the lookup as
        //       missing).
        //   * TTL exists and is live → fall through, include in host
        //     inputs as normal.
        //   * TTL is missing for a Soroban key → fall through; the
        //     host's e2e_invoke will treat the slot as absent, matching
        //     the legacy "key didn't exist in storage" semantics. The
        //     hot-archive auto-restore path is handled separately and
        //     not reached here.
        let entry_is_archived = match (k, &ttl_entry) {
            (LedgerKey::ContractData(d), Some(t)) => {
                if t.live_until_ledger_seq < ledger_seq {
                    Some(d.durability == ContractDataDurability::Persistent)
                } else {
                    None
                }
            }
            (LedgerKey::ContractCode(_), Some(t)) => {
                // Contract code is always persistent.
                if t.live_until_ledger_seq < ledger_seq {
                    Some(true)
                } else {
                    None
                }
            }
            _ => None,
        };
        match entry_is_archived {
            Some(true) => {
                // Persistent expired → fail the TX with ENTRY_ARCHIVED.
                return Ok(make_tx_failure_result(
                    SorobanTxFailure::EntryArchived,
                    Vec::new(),
                ));
            }
            Some(false) => {
                // Temporary expired → skip the slot.
                continue;
            }
            None => {} // Live or non-Soroban — fall through.
        }
        // Per-entry size cap (mirrors legacy validateContractLedgerEntry
        // pre-host call site): a CONTRACT_DATA / CONTRACT_CODE entry that
        // exceeds the network-config size limit fails the TX with
        // RESOURCE_LIMIT_EXCEEDED before the host even runs, even on the
        // read path. Triggered when the network config gets reduced
        // after entries were written above the new limit.
        //
        // Size resolution order:
        //   1. Cluster-local write: an earlier TX in this cluster wrote
        //      to the key via the host path, which always pairs the
        //      typed entry with its encoded bytes in `host_bytes`. The
        //      bytes' length is the entry's XDR size.
        //   2. State-resident (no shadow): use the cached xdr_size from
        //      SorobanState — avoids 5-50us per CONTRACT_CODE entry that
        //      the host doesn't need.
        //   3. Cross-stage write (or anywhere else): encode now. Rare on
        //      the apply hot path; not worth threading the phase-level
        //      `accumulated_host_bytes` through the invoke driver.
        let entry_size = if let Some(bytes) = host_bytes.get(*k) {
            bytes.len() as u32
        } else if cross_stage_writes.contains_key(*k) {
            xdr_serialized_size(&entry_cow)
        } else {
            state
                .cached_xdr_size_for(k)
                .unwrap_or_else(|| xdr_serialized_size(&entry_cow))
        };
        let cap_exceeded = match &entry_cow.data {
            LedgerEntryData::ContractData(_) => {
                entry_size > ledger_info.max_contract_data_entry_size_bytes
            }
            LedgerEntryData::ContractCode(_) => {
                entry_size > ledger_info.max_contract_size_bytes
            }
            _ => false,
        };
        if cap_exceeded {
            return Ok(make_tx_failure_result(
                SorobanTxFailure::ResourceLimitExceeded,
                Vec::new(),
            ));
        }
        // Classic entries (non-Soroban) are counted toward disk read bytes.
        let counts_toward_disk_read = !matches!(
            entry_cow.data,
            LedgerEntryData::ContractData(_)
                | LedgerEntryData::ContractCode(_)
                | LedgerEntryData::Ttl(_)
        );
        if counts_toward_disk_read {
            disk_read_bytes_consumed = disk_read_bytes_consumed.saturating_add(entry_size);
        }
        // Per-cluster Rc cache for state-resident CONTRACT_CODE and
        // CONTRACT_DATA entries. Many TXs in a cluster typically share
        // the same contract code (often tens of KB of WASM) AND a small
        // set of hot ContractData entries (e.g. contract instance /
        // global state). Caching avoids a full deep clone of the
        // LedgerEntry tree on every read.
        //
        // Restricted to ContractCode/ContractData: classic footprint
        // kinds (Account, Trustline) typically vary per TX, so a cache
        // lookup + miss-insert + LedgerKey clone would be pure overhead.
        // TTL keys read here aren't typed_ledger_entries values either.
        //
        // The cache only feeds unshadowed reads — once a key is
        // shadowed by a cluster-local / cross-stage write, the layered
        // value takes over. The cache itself is never invalidated
        // because state is immutable for the duration of the apply
        // phase, and we only cache state-sourced entries.
        let is_unshadowed = !cluster_local_writes.contains_key(*k)
            && !cross_stage_writes.contains_key(*k);
        // Cache is keyed by TtlKeyHash ([u8;32]) so the insert side
        // doesn't pay for a deep `LedgerKey::clone()` (which would
        // clone the inner SCVal / ContractCode hash). The hash was
        // already computed above for the TTL lookup; reuse it here.
        let entry_rc = if is_unshadowed && key_hash_for_soroban.is_some() {
            let key_hash = key_hash_for_soroban.unwrap();
            if let Some(cached) = state_entry_rc_cache.get(&key_hash) {
                std::rc::Rc::clone(cached)
            } else {
                let owned = entry_cow.into_owned();
                let rc = std::rc::Rc::new(owned);
                state_entry_rc_cache.insert(key_hash, std::rc::Rc::clone(&rc));
                rc
            }
        } else {
            std::rc::Rc::new(entry_cow.into_owned())
        };
        typed_ledger_entries.push((entry_rc, ttl_entry, entry_size));
    }
    if disk_read_bytes_consumed > disk_read_bytes_limit {
        // Resource limit exceeded — the TX must fail with
        // RESOURCE_LIMIT_EXCEEDED before the host runs, so its writes
        // never enter cluster_local_writes.
        return Ok(make_tx_failure_result(
            SorobanTxFailure::ResourceLimitExceeded,
            Vec::new(),
        ));
    }

    // Per-TX PRNG seed: derived by the orchestrator as
    // SHA256(soroban_base_prng_seed || tx_num_be) and passed in here.
    // Mirrors the C++ subSha256 derivation that
    // TransactionFrame::doApply / parallelApply used to produce.
    let prng_seed = *per_tx_prng_seed;

    // Auto-restored RW indices come from the resourceExt's
    // archivedSorobanEntries: the host treats footprint slots at these
    // indices as freshly-restored from the hot archive (rent computed
    // against an old liveUntil of 0, fresh module compile if applicable).
    // We keep auto_restore_rw_indices for the post-pass markRestored
    // bookkeeping, and re-borrow it for the host call (host wants
    // `&[u32]`, no need to clone the Vec).
    let restored_rw_entry_indices: &[u32] = auto_restore_rw_indices.as_slice();

    // Protocol-gated dispatch:
    //  * V_26 (the only protocol soroban_curr/p26 supports) takes the
    //    fast typed path — host consumes typed inputs straight, no
    //    per-input XDR encode/decode. Each owned input (host_function,
    //    auth_entries, source_account) is moved straight into the host
    //    call.
    //  * V_21..V_25 fall back to the bytes path; their pinned env
    //    crates have only the bytes entry point. The same owned inputs
    //    are borrowed for the bytes encoder.
    let typed_path = ledger_info.protocol_version
        == crate::soroban_proto_all::soroban_curr::soroban_proto_any::get_max_proto();
    let mut result = if typed_path {
        let source_account_id = muxed_to_account_id_owned(source_account);
        // Hand typed_ledger_entries by-move: the host (e2e_invoke)
        // takes ownership and moves entries into Rcs without an
        // additional metered_clone. Saves ~5us per CONTRACT_CODE
        // input on the apply hot path. When diagnostics are on,
        // we still need the typed entries afterwards for
        // post-host metrics — clone to satisfy that branch.
        let entries_for_host = if enable_diagnostics {
            typed_ledger_entries.clone()
        } else {
            std::mem::take(&mut typed_ledger_entries)
        };
        invoke_host_function_typed_curr(
            enable_diagnostics,
            resources.instructions,
            host_function,
            resources.clone(),
            restored_rw_entry_indices,
            source_account_id,
            auth_entries,
            ledger_info,
            entries_for_host,
            prng_seed,
            rent_fee_configuration,
            module_cache,
        )?
    } else {
        // Bytes path: serialize the typed footprint once at the boundary.
        let mut ledger_entry_bufs: Vec<CxxBuf> =
            Vec::with_capacity(typed_ledger_entries.len());
        let mut ttl_entry_bufs: Vec<CxxBuf> =
            Vec::with_capacity(typed_ledger_entries.len());
        for (entry, ttl, _size) in &typed_ledger_entries {
            ledger_entry_bufs.push(xdr_to_cxx_buf(entry.as_ref()));
            ttl_entry_bufs.push(match ttl {
                Some(t) => xdr_to_cxx_buf(t),
                None => bytes_to_cxx_buf(&[]),
            });
        }
        invoke_host_function_old_env_serialized(
            config_max_protocol,
            enable_diagnostics,
            resources.instructions,
            &host_function,
            resources,
            restored_rw_entry_indices,
            &source_account,
            &auth_entries,
            ledger_info,
            ledger_entry_bufs,
            ttl_entry_bufs,
            &prng_seed,
            rent_fee_configuration,
            module_cache,
        )?
    };

    // Budget check: if the host succeeded but the rent it computed
    // exceeds the TX's max_refundable_fee (declared - non_refundable),
    // bail BEFORE folding writes into cluster_local_writes. The legacy
    // path relied on its surrounding LedgerTxn rolling back the host's
    // mutations when consumeRefundableSorobanResources returned false;
    // doing the equivalent in the new orchestrator means simply not
    // merging the writes here so subsequent TXs don't observe them and
    // so accumulated_writes / SorobanState stay clean. The companion
    // C++ post-pass still runs consumeRefundableSorobanResources, sees
    // is_insufficient_refundable_fee=true, and surfaces the
    // INSUFFICIENT_REFUNDABLE_FEE op result code.
    if result.success && result.rent_fee > max_refundable_fee {
        return Ok(make_tx_failure_result(
            SorobanTxFailure::InsufficientRefundableFee,
            result.diagnostic_events_xdr.into_iter().map(RustBuf::from).collect(),
        ));
    }

    // Capture per-TX deltas BEFORE folding new entries into
    // cluster_local_writes — otherwise layered_get would return the new
    // value as "prev". Then fold so subsequent TXs see the writes.
    //
    // Deletion convention: the host signals "RW key was deleted" by
    // OMITTING the entry from `modified_ledger_entries` (see
    // soroban_proto_any::extract_ledger_effects). Walk the RW footprint
    // afterwards: any RW key absent from the modified set is a
    // tombstone, and its accompanying TTL must also be tombstoned.
    //
    // The host's modified_ledger_entries come back with
    // last_modified_ledger_seq left at zero. Mirror the legacy C++
    // LedgerTxn::maybeUpdateLastModified bump to the current ledger
    // here so InMemorySorobanState doesn't end up with half-populated
    // TtlData (live_until > 0, last_modified == 0), and so bucket
    // writeback sees the right value.
    //
    // Classic entries (Account / Trustline / etc.) emitted by the
    // Soroban host as side effects of native asset ops flow through
    // cluster_local_writes the same way Soroban entries do;
    // apply_phase_writes_to_state routes them as plain ledger_updates
    // without touching SorobanState.
    // Write-bytes resource check (mirrors the legacy
    // InvokeHostFunctionOpFrame post-host loop at lines ~700-715
    // of the gut'd commit). For each non-TTL modified entry the host
    // returned, count keySize + entry XDR size; if the running sum
    // exceeds resources.writeBytes, surface RESOURCE_LIMIT_EXCEEDED
    // and drop all writes (don't fold into cluster_local_writes).
    // Also enforce the per-entry size caps from the network config
    // (maxContractSizeBytes for CONTRACT_CODE,
    // maxContractDataEntrySizeBytes for CONTRACT_DATA) — mirrors the
    // legacy validateContractLedgerEntry call at the top of that
    // loop. The host's e2e_invoke does not enforce these caps in
    // production mode (verify_limits is recording-mode only).
    if result.success {
        // Mirrors legacy noteWriteEntry: mLedgerWriteByte += entrySize
        // only (keySize is tracked separately for max-key-byte stats,
        // not against the writeBytes cap). The host already encoded
        // each modified entry — use that byte length instead of
        // re-serializing.
        let mut write_bytes_used: u32 = 0;
        let mut per_entry_cap_exceeded = false;
        for (entry, encoded) in result.modified_ledger_entries.iter() {
            if matches!(&entry.data, LedgerEntryData::Ttl(_)) {
                continue;
            }
            let entry_size = encoded.len() as u32;
            match &entry.data {
                LedgerEntryData::ContractData(_) => {
                    if entry_size > ledger_info.max_contract_data_entry_size_bytes {
                        per_entry_cap_exceeded = true;
                    }
                }
                LedgerEntryData::ContractCode(_) => {
                    if entry_size > ledger_info.max_contract_size_bytes {
                        per_entry_cap_exceeded = true;
                    }
                }
                _ => {}
            }
            write_bytes_used = write_bytes_used.saturating_add(entry_size);
        }
        if per_entry_cap_exceeded || write_bytes_used > resources.write_bytes {
            return Ok(make_tx_failure_result(
                SorobanTxFailure::ResourceLimitExceeded,
                result.diagnostic_events_xdr.into_iter().map(RustBuf::from).collect(),
            ));
        }
    }

    let mut tx_changes: Vec<crate::LedgerEntryDelta> = Vec::new();
    if result.success {
        // Pre-compute the set of TTL keys whose underlying data/code key
        // is in this TX's RW footprint. TTL entries returned by the host
        // for these keys flow into cluster_local_writes (immediately
        // visible to subsequent TXs in the cluster); TTL entries for
        // keys outside this set are RO TTL bumps and go into
        // ro_ttl_bumps (buffered, max-merged at cluster end).
        let mut rw_ttl_keys: FastSet<LedgerKey> = FastSet::default();
        for k in footprint.read_write.iter() {
            if matches!(
                k,
                LedgerKey::ContractData(_) | LedgerKey::ContractCode(_)
            ) {
                rw_ttl_keys.insert(ttl_lookup_key_for(k));
            }
        }
        // Track which RW Soroban keys came back from the host so we can
        // later detect deletes (RW key in footprint, missing from
        // modified_ledger_entries).
        let mut returned_rw_keys: FastSet<LedgerKey> = FastSet::default();
        let modified_entries = std::mem::take(&mut result.modified_ledger_entries);
        tx_changes.reserve(modified_entries.len());
        for (mut owned, mut encoded) in modified_entries.into_iter() {
            owned.last_modified_ledger_seq = ledger_info.sequence_number;
            // Patch the encoded form to match the bumped seq so the
            // bytes the host gave us can be reused verbatim by
            // apply_phase_writes_to_state and build_tx_delta.
            patch_last_modified_seq(&mut encoded, ledger_info.sequence_number);
            let key = ledger_entry_key(&owned);
            if matches!(
                &owned.data,
                LedgerEntryData::ContractData(_)
                    | LedgerEntryData::ContractCode(_)
            ) {
                returned_rw_keys.insert(key.clone());
            }
            // RO TTL bumps: the entry is a TTL entry and its underlying
            // data/code key is NOT in this TX's RW footprint. Capture
            // tx_changes (so meta still records this TX's contribution),
            // then route into ro_ttl_bumps for cluster-end max-merge.
            let is_ro_ttl_bump = matches!(&owned.data, LedgerEntryData::Ttl(_))
                && !rw_ttl_keys.contains(&key);
            // Skip the meta-side delta when tx-meta is disabled — the
            // C++ post-pass would discard it. Saves three XDR encodes
            // per modified entry (key, prev, new) on the hot path.
            if enable_tx_meta {
                tx_changes.push(build_tx_delta_with_cached_new(
                    state,
                    cross_stage_writes,
                    cluster_local_writes,
                    classic_prefetch,
                    &key,
                    Some(&owned),
                    Some(&encoded),
                )?);
            }
            if is_ro_ttl_bump {
                // Bytes flow alongside the typed entry inside
                // ro_ttl_bumps so the phase-end commit can reuse the
                // host-supplied encoding instead of re-serializing.
                // The pair stays in sync: when buffered_higher wins
                // we keep BOTH the prior typed entry and its bytes;
                // when the incoming bump wins we overwrite both. The
                // cluster-end / pre-RW-flush drains then carry the
                // winning bytes into host_bytes.
                let bumped_live_until = match &owned.data {
                    LedgerEntryData::Ttl(t) => t.live_until_ledger_seq,
                    _ => unreachable!(),
                };
                let buffered_higher = ro_ttl_bumps
                    .get(&key)
                    .and_then(|(le, _)| match &le.data {
                        LedgerEntryData::Ttl(t) => Some(t.live_until_ledger_seq),
                        _ => None,
                    })
                    .map(|prev| prev >= bumped_live_until)
                    .unwrap_or(false);
                if !buffered_higher {
                    ro_ttl_bumps.insert(key, (owned, encoded));
                }
            } else {
                // RW write: stash the host-supplied bytes alongside
                // the typed entry. apply_phase_writes_to_state hands
                // them straight to the LedgerEntryUpdate, skipping a
                // re-encode. Within the cluster, later writes to the
                // same key overwrite — only the latest write reaches
                // the bucket, and the bytes for that latest write are
                // what we want to emit.
                host_bytes.insert(key.clone(), encoded);
                cluster_local_writes.insert(key, Some(owned));
            }
        }

        // Auto-restored keys this TX touched: if the host then deletes
        // them (omits from modified_ledger_entries), we still need to
        // tombstone in cluster_local_writes so subsequent TXs see
        // "deleted" rather than "archived" (the static
        // archived_prefetch still has the entry).
        let auto_restored_keys_set: FastSet<LedgerKey> =
            auto_restored_data_writes
                .iter()
                .map(|(k, _)| k.clone())
                .chain(auto_restored_live_data.iter().map(|(k, _)| k.clone()))
                .collect();
        // Detect deletions: RW Soroban keys whose entries the host did
        // NOT return are tombstones. Their TTL entries must also be
        // tombstoned to keep state consistent.
        for k in footprint.read_write.iter() {
            match k {
                LedgerKey::ContractData(_) | LedgerKey::ContractCode(_) => {
                    if returned_rw_keys.contains(k) {
                        continue;
                    }
                    // Only emit a tombstone if the entry actually existed
                    // — either in state / cross-stage / cluster-local
                    // OR auto-restored by THIS TX (visible to host but
                    // not yet folded into cluster_local_writes).
                    let prev = layered_get(
                        state,
                        cross_stage_writes,
                        cluster_local_writes,
                        classic_prefetch,
                        k,
                    );
                    if prev.is_none() && !auto_restored_keys_set.contains(k) {
                        continue;
                    }
                    if enable_tx_meta {
                        tx_changes.push(build_tx_delta(
                            state,
                            cross_stage_writes,
                            cluster_local_writes,
                            classic_prefetch,
                            k,
                            None,
                        )?);
                    }
                    cluster_local_writes.insert(k.clone(), None);
                    let ttl_key = ttl_lookup_key_for(k);
                    let ttl_prev = layered_get(
                        state,
                        cross_stage_writes,
                        cluster_local_writes,
                        classic_prefetch,
                        &ttl_key,
                    );
                    if ttl_prev.is_some() || auto_restored_keys_set.contains(k) {
                        if enable_tx_meta {
                            tx_changes.push(build_tx_delta(
                                state,
                                cross_stage_writes,
                                cluster_local_writes,
                                classic_prefetch,
                                &ttl_key,
                                None,
                            )?);
                        }
                        cluster_local_writes.insert(ttl_key, None);
                    }
                }
                _ => {}
            }
        }
    }

    // Compute and append "core_metrics" diagnostic events to mirror
    // the legacy InvokeHostFunctionOpFrame::publishMetricsEvents path.
    // Mirrors the legacy `cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS` gate:
    // when disabled, skip the per-tx XDR encode of the 19 metric events.
    if enable_diagnostics {
        append_core_metrics_for_invocation(&mut result, &footprint_keys, &typed_ledger_entries);
    }

    // The C++ side wants XDR bytes for the return value (to populate
    // InvokeHostFunctionResult.success and feed the success-hash
    // preimage). The host already gave them to us as bytes; pass them
    // straight through — no decode/re-encode roundtrip.
    let return_value_xdr = RustBuf::from(std::mem::take(&mut result.return_value_xdr));

    // Fee-refund inputs: rent_fee comes straight from the host; event
    // size is the sum of XDR-serialised ContractEvent bytes PLUS the
    // size of the InvokeHostFunction result_value (the legacy
    // collectEvents path in the C++ op-frame folded both into
    // mEmitEventByte before passing it to consumeRefundableSorobanResources;
    // missing the return-value byte count under-charges the events fee
    // and inflates the rent fee residual the test exercises). TTL /
    // Restore ops emit no events and have no return value so this
    // expression is 0 there.
    let contract_event_size_bytes: u32 = if result.success {
        let events_sum: u32 = result
            .contract_events_xdr
            .iter()
            .map(|v| v.len() as u32)
            .sum();
        events_sum + return_value_xdr.data.len() as u32
    } else {
        0
    };
    let rent_fee_consumed = if result.success { result.rent_fee } else { 0 };

    // Map post-host failures to RESOURCE_LIMIT_EXCEEDED when the cause
    // was clearly a CPU / memory budget overrun. Mirrors the legacy
    // doApply post-host check: if the host failed and it consumed more
    // CPU than declared, surface RESOURCE_LIMIT_EXCEEDED rather than
    // the generic TRAPPED. Without this, tests that intentionally
    // construct a TX with a tight resource budget see TRAPPED instead
    // of the resource-limit code they expect.
    let host_resource_limit_exceeded = !result.success
        && !result.is_internal_error
        && (result.cpu_insns > u64::from(resources.instructions)
            || result.mem_bytes > ledger_info.memory_limit as u64);

    // Build hot_archive_restores / live_restores from the auto-restore
    // loads we did at footprint-walk time. Each (data, ttl) pair is
    // reported as two LedgerEntryUpdates the C++ post-pass groups by
    // TTL key hash to call markRestoredFromHotArchive /
    // markRestoredFromLiveBucketList. Only emit on success — a failed
    // host invocation rolls back all writes, including auto-restores.
    let (hot_archive_restores, live_restores) = if result.success {
        build_auto_restore_records(
            &auto_restored_data_writes,
            &auto_restored_ttl_writes,
            &auto_restored_live_data,
            &auto_restored_live_ttl,
        )?
    } else {
        (Vec::new(), Vec::new())
    };

    let success_preimage_hash = compute_success_preimage_hash(
        result.success,
        &return_value_xdr.data,
        &result.contract_events_xdr,
    );

    // Pre-compute the events-portion of the resource fee on the Rust
    // side so the C++ post-pass doesn't need to call back into Rust via
    // `RefundableFeeTracker::consumeRefundableSorobanResources` →
    // `computeSorobanResourceFee` FFI.
    let refundable_fee_increment = if result.success {
        crate::soroban_apply::common::compute_refundable_fee_increment(
            config_max_protocol,
            ledger_info.protocol_version,
            resources,
            archived_rw_indices.len() as u32,
            /*is_restore_footprint_op=*/ false,
            tx_envelope_size_bytes,
            contract_event_size_bytes,
            fee_configuration,
        )?
    } else {
        0
    };

    Ok(crate::SorobanTxApplyResult {
        success: result.success,
        is_internal_error: result.is_internal_error,
        is_insufficient_refundable_fee: false,
        is_resource_limit_exceeded: host_resource_limit_exceeded,
        is_entry_archived: false,
        return_value_xdr,
        contract_events: result
            .contract_events_xdr
            .into_iter()
            .map(RustBuf::from)
            .collect(),
        diagnostic_events: result
            .diagnostic_events_xdr
            .into_iter()
            .map(RustBuf::from)
            .collect(),
        rent_fee_consumed,
        contract_event_size_bytes,
        tx_changes,
        hot_archive_restores,
        live_restores,
        success_preimage_hash,
        refundable_fee_increment,
    })
}

// Accumulate per-TX read / write / event byte counts from the footprint
// inputs and host outputs into an InvokeMetrics, then append the 19
// "core_metrics" diagnostic events to result.diagnostic_events_xdr.
// Mirrors the legacy InvokeHostFunctionOpFrame::publishMetricsEvents
// shape.
fn append_core_metrics_for_invocation(
    result: &mut InvokeHostFunctionTypedResult,
    footprint_keys: &[&LedgerKey],
    typed_ledger_entries: &[(std::rc::Rc<LedgerEntry>, Option<TtlEntry>, u32)],
) {
    let mut metrics = InvokeMetrics::default();
    metrics.cpu_insn = result.cpu_insns;
    metrics.mem_byte = result.mem_bytes;
    metrics.invoke_time_nsecs = result.time_nsecs;
    // Read-side accounting walks the footprint as it was passed to the
    // host (RO + RW that were actually loaded / auto-restored).
    metrics.read_entry = typed_ledger_entries.len() as u32;
    for k in footprint_keys {
        let key_size: u32 = xdr_to_vec(*k)
            .map(|b| b.len() as u32)
            .unwrap_or(0);
        metrics.read_key_byte = metrics.read_key_byte.saturating_add(key_size);
        metrics.max_rw_key_byte = metrics.max_rw_key_byte.max(key_size);
    }
    for (entry, _ttl, entry_size) in typed_ledger_entries {
        let entry_size = *entry_size;
        metrics.ledger_read_byte = metrics.ledger_read_byte.saturating_add(entry_size);
        match &entry.data {
            LedgerEntryData::ContractData(_) => {
                metrics.read_data_byte = metrics.read_data_byte.saturating_add(entry_size);
                metrics.max_rw_data_byte = metrics.max_rw_data_byte.max(entry_size);
            }
            LedgerEntryData::ContractCode(_) => {
                metrics.read_code_byte = metrics.read_code_byte.saturating_add(entry_size);
                metrics.max_rw_code_byte = metrics.max_rw_code_byte.max(entry_size);
            }
            _ => {}
        }
    }
    // Write-side accounting from the host's modified ledger entries.
    // Note: by the time this runs the post-host fold loop has drained
    // modified_ledger_entries, so this is typically a no-op; kept for
    // shape-compat with the legacy bytes path's ordering. When entries
    // are present they get counted.
    for (entry, encoded) in result.modified_ledger_entries.iter() {
        if matches!(&entry.data, LedgerEntryData::Ttl(_)) {
            continue;
        }
        let entry_size = encoded.len() as u32;
        let key = ledger_entry_key(entry);
        let key_size: u32 = xdr_to_vec(&key)
            .map(|b| b.len() as u32)
            .unwrap_or(0);
        metrics.write_entry = metrics.write_entry.saturating_add(1);
        metrics.write_key_byte = metrics.write_key_byte.saturating_add(key_size);
        metrics.max_rw_key_byte = metrics.max_rw_key_byte.max(key_size);
        metrics.ledger_write_byte = metrics.ledger_write_byte.saturating_add(entry_size);
        match &entry.data {
            LedgerEntryData::ContractData(_) => {
                metrics.write_data_byte = metrics.write_data_byte.saturating_add(entry_size);
                metrics.max_rw_data_byte = metrics.max_rw_data_byte.max(entry_size);
            }
            LedgerEntryData::ContractCode(_) => {
                metrics.write_code_byte = metrics.write_code_byte.saturating_add(entry_size);
                metrics.max_rw_code_byte = metrics.max_rw_code_byte.max(entry_size);
            }
            _ => {}
        }
    }
    metrics.emit_event = result.contract_events_xdr.len() as u32;
    for ev in result.contract_events_xdr.iter() {
        let event_size = ev.len() as u32;
        metrics.emit_event_byte = metrics.emit_event_byte.saturating_add(event_size);
        metrics.max_emit_event_byte = metrics.max_emit_event_byte.max(event_size);
    }
    append_core_metrics_events(
        &mut result.diagnostic_events_xdr,
        result.success,
        &metrics,
    );
}

// Emit hot_archive_restores / live_restores from the auto-restore loads
// done at footprint-walk time. The C++ post-pass groups by TTL key hash
// to call markRestoredFromHotArchive / markRestoredFromLiveBucketList.
fn build_auto_restore_records(
    auto_restored_data_writes: &[(LedgerKey, std::rc::Rc<LedgerEntry>)],
    auto_restored_ttl_writes: &[(LedgerKey, LedgerEntry)],
    auto_restored_live_data: &[(LedgerKey, std::rc::Rc<LedgerEntry>)],
    auto_restored_live_ttl: &[(LedgerKey, LedgerEntry)],
) -> Result<(Vec<LedgerEntryUpdate>, Vec<LedgerEntryUpdate>), Box<dyn std::error::Error>> {
    fn push_pairs_rc(
        target: &mut Vec<LedgerEntryUpdate>,
        pairs: &[(LedgerKey, std::rc::Rc<LedgerEntry>)],
    ) -> Result<(), Box<dyn std::error::Error>> {
        for (key, entry) in pairs {
            target.push(LedgerEntryUpdate {
                key_xdr: RustBuf::from(xdr_to_vec(key)?),
                value_xdr: RustBuf::from(xdr_to_vec(entry.as_ref())?),
            });
        }
        Ok(())
    }
    fn push_pairs(
        target: &mut Vec<LedgerEntryUpdate>,
        pairs: &[(LedgerKey, LedgerEntry)],
    ) -> Result<(), Box<dyn std::error::Error>> {
        for (key, entry) in pairs {
            target.push(LedgerEntryUpdate {
                key_xdr: RustBuf::from(xdr_to_vec(key)?),
                value_xdr: RustBuf::from(xdr_to_vec(entry)?),
            });
        }
        Ok(())
    }
    let mut hot_archive_restores = Vec::new();
    push_pairs_rc(&mut hot_archive_restores, auto_restored_data_writes)?;
    push_pairs(&mut hot_archive_restores, auto_restored_ttl_writes)?;
    let mut live_restores = Vec::new();
    push_pairs_rc(&mut live_restores, auto_restored_live_data)?;
    push_pairs(&mut live_restores, auto_restored_live_ttl)?;
    Ok((hot_archive_restores, live_restores))
}

// SHA-256 of the InvokeHostFunctionSuccessPreImage XDR
// `(SCVal, ContractEvent<>)`: SCVal bytes, then a 4-byte big-endian
// event count, then each event's XDR bytes concatenated. Returns an
// empty RustBuf on failure (the preimage is only valid on success).
fn compute_success_preimage_hash(
    success: bool,
    return_value_xdr: &[u8],
    contract_events_xdr: &[Vec<u8>],
) -> RustBuf {
    if !success {
        return RustBuf::from(Vec::<u8>::new());
    }
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    h.update(return_value_xdr);
    let n = contract_events_xdr.len() as u32;
    h.update(&n.to_be_bytes());
    for ev in contract_events_xdr {
        h.update(ev);
    }
    let bytes: [u8; 32] = h.finalize().into();
    RustBuf::from(bytes.to_vec())
}

// ExtendFootprintTtl per-TX wiring. Builds slots from layered state for
// each read-only footprint key whose entry + TTL are present and live;
// calls extend_footprint_ttl_old_env (C8); folds the bumped TTLs back
