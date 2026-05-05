// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! RestoreFootprint op driver. For each read-write footprint slot
//! whose entry has an expired TTL or lives in the hot archive, restores
//! the entry to the live BL with a fresh TTL and computes the
//! restoration rent fee.

use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::{
    ContractDataDurability, Hash, LedgerEntry, LedgerEntryData, LedgerEntryExt, LedgerKey, Limits,
    SorobanResources, TtlEntry, WriteXdr,
};

use super::common::{
    build_tx_delta, compute_contract_code_size_for_rent, layered_get, ledger_entry_key,
    make_tx_failure_result, ttl_key_hash_for, ttl_lookup_key_for, xdr_serialized_size,
    AccumulatedWrites, FastMap, SorobanTxFailure,
};
use super::state::SorobanState;
use crate::{
    CxxLedgerEntryRentChange, CxxLedgerInfo, CxxRentFeeConfiguration, LedgerEntryUpdate,
    RustBuf,
};

enum RestoreSource<'a> {
    Live(&'a LedgerEntry),
    HotArchive(&'a LedgerEntry),
}

struct RestoreFootprintSlot<'a> {
    // The CONTRACT_DATA / CONTRACT_CODE LedgerKey being restored.
    ledger_key: &'a LedgerKey,
    // Where the entry comes from.
    source: RestoreSource<'a>,
}

struct RestoreFootprintOutput {
    // Restored CONTRACT_DATA / CONTRACT_CODE LedgerEntries (with
    // lastModifiedLedgerSeq bumped to the current ledger).
    restored_entries: Vec<LedgerEntry>,
    // New TTL LedgerEntries for each restored entry.
    new_ttl_entries: Vec<LedgerEntry>,
    rent_fee: i64,
}

fn restore_footprint_old_env(
    config_max_protocol: u32,
    protocol_version: u32,
    current_ledger_seq: u32,
    min_persistent_ttl: u32,
    rent_fee_configuration: CxxRentFeeConfiguration,
    cpu_cost_params: &[u8],
    mem_cost_params: &[u8],
    slots: &[RestoreFootprintSlot<'_>],
) -> Result<RestoreFootprintOutput, Box<dyn std::error::Error>> {
    // Restored entries are extended to cover the minimum persistent TTL
    // including the current ledger. Same formula as C++:
    //   restoredLiveUntilLedger = ledgerSeq + minPersistentTTL - 1
    let restored_live_until =
        current_ledger_seq.saturating_add(min_persistent_ttl).saturating_sub(1);

    let mut restored_entries = Vec::with_capacity(slots.len());
    let mut new_ttl_entries = Vec::with_capacity(slots.len());
    let mut rent_changes: Vec<CxxLedgerEntryRentChange> = Vec::with_capacity(slots.len());

    for slot in slots {
        let mut entry = match slot.source {
            RestoreSource::Live(e) | RestoreSource::HotArchive(e) => e.clone(),
        };
        // Match C++: bump lastModifiedLedgerSeq on the restored entry.
        entry.last_modified_ledger_seq = current_ledger_seq;

        let (is_persistent, is_code_entry) = match &entry.data {
            LedgerEntryData::ContractData(d) => (
                d.durability == ContractDataDurability::Persistent,
                false,
            ),
            LedgerEntryData::ContractCode(_) => (true, true),
            _ => {
                return Err(
                    "restore_footprint_old_env: restored entry is not CONTRACT_DATA or CONTRACT_CODE"
                        .into(),
                );
            }
        };
        // Rent-aware size: for protocol >= 23 + CONTRACT_CODE, this is
        // xdr_size + parsed-module memory footprint; otherwise it's
        // plain xdr_size. Mirrors C++ `ledgerEntrySizeForRent`.
        let entry_size = if is_code_entry {
            compute_contract_code_size_for_rent(
                &entry,
                config_max_protocol,
                protocol_version,
                cpu_cost_params,
                mem_cost_params,
            )
        } else {
            xdr_serialized_size(&entry)
        };

        // Restoration is treated as a creation for rent purposes — old TTL
        // is 0 (the entry was archived), new TTL is the restored extension.
        rent_changes.push(CxxLedgerEntryRentChange {
            is_persistent,
            is_code_entry,
            old_size_bytes: 0,
            new_size_bytes: entry_size,
            old_live_until_ledger: 0,
            new_live_until_ledger: restored_live_until,
        });

        // Build the new TTL LedgerEntry for this slot.
        let key_hash = ttl_key_hash_for(slot.ledger_key);
        let new_ttl = LedgerEntry {
            last_modified_ledger_seq: current_ledger_seq,
            data: LedgerEntryData::Ttl(TtlEntry {
                key_hash: Hash(key_hash),
                live_until_ledger_seq: restored_live_until,
            }),
            ext: LedgerEntryExt::V0,
        };

        restored_entries.push(entry);
        new_ttl_entries.push(new_ttl);
    }

    let rent_fee = crate::soroban_invoke::compute_rent_fee(
        config_max_protocol,
        protocol_version,
        &rent_changes,
        rent_fee_configuration,
        current_ledger_seq,
    )?;

    Ok(RestoreFootprintOutput {
        restored_entries,
        new_ttl_entries,
        rent_fee,
    })
}

// Per-TX driver for the RestoreFootprint op: resolves each RW slot from
// either the live state with an expired TTL or the hot archive, calls the
// per-protocol rent calculator, and folds the restored entries / new TTLs
// back into the cluster-local accumulator. Mirrors the legacy C++
// RestoreFootprintOpFrame::doApply flow.
pub(super) fn apply_restore_footprint(
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &mut AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    archived_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    config_max_protocol: u32,
    ledger_info: &CxxLedgerInfo,
    rent_fee_configuration: CxxRentFeeConfiguration,
    resources: &SorobanResources,
    max_refundable_fee: i64,
) -> Result<crate::SorobanTxApplyResult, Box<dyn std::error::Error>> {
    let current_ledger_seq = ledger_info.sequence_number;
    // Restored entries get extended to cover at least the network's
    // min_persistent_ttl. Real value is in SorobanNetworkConfig; for now
    // we use the protocol-default minimum from CxxLedgerInfo.
    let min_persistent_ttl = ledger_info.min_persistent_entry_ttl;

    let owned_sources = plan_restore_sources(
        resources,
        state,
        cross_stage_writes,
        cluster_local_writes,
        classic_prefetch,
        archived_prefetch,
        current_ledger_seq,
    );
    if let Some(failure) = check_restore_resource_limits(&owned_sources, ledger_info, resources) {
        return Ok(failure);
    }
    let slots: Vec<RestoreFootprintSlot<'_>> = owned_sources
        .iter()
        .map(|(k, e, is_archived)| RestoreFootprintSlot {
            ledger_key: k,
            source: if *is_archived {
                RestoreSource::HotArchive(e)
            } else {
                RestoreSource::Live(e)
            },
        })
        .collect();

    let output = restore_footprint_old_env(
        config_max_protocol,
        ledger_info.protocol_version,
        current_ledger_seq,
        min_persistent_ttl,
        rent_fee_configuration,
        ledger_info.cpu_cost_params.as_ref(),
        ledger_info.mem_cost_params.as_ref(),
        &slots,
    )?;

    // Refundable-fee budget check — mirrors the legacy
    // RestoreFootprintOpFrame consumeRefundableResources path. If the
    // computed rent fee exceeds the TX's max refundable budget, fail
    // before folding any writes (state and bucket writeback stay
    // clean; sibling TXs don't observe the half-restored entries).
    if output.rent_fee > max_refundable_fee {
        return Ok(make_tx_failure_result(
            SorobanTxFailure::InsufficientRefundableFee,
            Vec::new(),
        ));
    }

    let (tx_changes, hot_archive_restores, live_restores) = fold_restored_entries(
        &output,
        &owned_sources,
        state,
        cross_stage_writes,
        cluster_local_writes,
        classic_prefetch,
    )?;

    // RestoreFootprint, like ExtendFootprintTtl, has no return value or
    // contract events. The rent fee is the cost of bringing the
    // restored entries back to live + a fresh minimum-TTL stamp.
    Ok(crate::SorobanTxApplyResult {
        success: true,
        is_internal_error: false,
        is_insufficient_refundable_fee: false,
        is_resource_limit_exceeded: false,
        is_entry_archived: false,
        return_value_xdr: RustBuf::from(Vec::<u8>::new()),
        contract_events: Vec::new(),
        diagnostic_events: Vec::new(),
        rent_fee_consumed: output.rent_fee,
        contract_event_size_bytes: 0,
        tx_changes,
        hot_archive_restores,
        live_restores,
        success_preimage_hash: RustBuf::from(Vec::<u8>::new()),
        refundable_fee_increment: 0,
    })
}

// Walk the RW footprint and collect (key, source entry, is_archived) for
// each slot the orchestrator wants to restore: an expired-but-still-live
// entry (Live) or an archived entry (HotArchive). Slots that aren't in
// either layer are silently skipped (C++ behaviour). Slots already
// tombstoned in this phase by a sibling auto-restore + delete are also
// skipped.
fn plan_restore_sources(
    resources: &SorobanResources,
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    archived_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    current_ledger_seq: u32,
) -> Vec<(LedgerKey, LedgerEntry, bool)> {
    let mut sources: Vec<(LedgerKey, LedgerEntry, bool)> = Vec::new();
    for k in resources.footprint.read_write.iter() {
        if !matches!(
            k,
            LedgerKey::ContractData(_) | LedgerKey::ContractCode(_)
        ) {
            continue;
        }
        // archived_prefetch is a static phase-time snapshot. If a sibling
        // TX already auto-restored + deleted the entry, the tombstone in
        // cluster_local_writes / cross_stage_writes tells us the entry no
        // longer exists. Skip the restore.
        let phase_deleted = cluster_local_writes
            .get(k)
            .map(|s| s.is_none())
            .unwrap_or(false)
            || cross_stage_writes
                .get(k)
                .map(|s| s.is_none())
                .unwrap_or(false);
        if phase_deleted {
            continue;
        }
        let ttl_key = ttl_lookup_key_for(k);
        let ttl_in_state = layered_get(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            &ttl_key,
        );
        match ttl_in_state {
            Some(ttl_entry) => {
                // Live state has the entry. Skip if already live.
                let LedgerEntryData::Ttl(ttl) = &ttl_entry.data else {
                    continue;
                };
                if ttl.live_until_ledger_seq >= current_ledger_seq {
                    continue;
                }
                // Expired: pull the data entry from live state.
                let Some(entry) = layered_get(
                    state,
                    cross_stage_writes,
                    cluster_local_writes,
                    classic_prefetch,
                    k,
                ) else {
                    continue;
                };
                sources.push((k.clone(), entry.into_owned(), false));
            }
            None => {
                // No live TTL: try the hot archive prefetch.
                let Some(archived) = archived_prefetch.get(k).cloned() else {
                    continue;
                };
                sources.push((k.clone(), archived, true));
            }
        }
    }
    sources
}

// Enforce per-entry size cap, disk-read bytes cap, and write-bytes cap on
// the planned restore sources. Returns a failed SorobanTxApplyResult if
// any cap is exceeded; None otherwise. Mirrors the legacy
// RestoreFootprintOpFrame metering checks (entries are read AND written,
// so each restored entry's XDR size counts toward both caps).
fn check_restore_resource_limits(
    owned_sources: &[(LedgerKey, LedgerEntry, bool)],
    ledger_info: &CxxLedgerInfo,
    resources: &SorobanResources,
) -> Option<crate::SorobanTxApplyResult> {
    let mut total_bytes: u32 = 0;
    for (_, entry, _) in owned_sources {
        let entry_size = xdr_serialized_size(entry);
        let cap_exceeded = match &entry.data {
            LedgerEntryData::ContractData(_) => {
                entry_size > ledger_info.max_contract_data_entry_size_bytes
            }
            LedgerEntryData::ContractCode(_) => {
                entry_size > ledger_info.max_contract_size_bytes
            }
            _ => false,
        };
        if cap_exceeded {
            return Some(make_tx_failure_result(
                SorobanTxFailure::ResourceLimitExceeded,
                Vec::new(),
            ));
        }
        total_bytes = total_bytes.saturating_add(entry_size);
    }
    if total_bytes > resources.disk_read_bytes || total_bytes > resources.write_bytes {
        return Some(make_tx_failure_result(
            SorobanTxFailure::ResourceLimitExceeded,
            Vec::new(),
        ));
    }
    None
}

// Capture per-TX deltas for each restored entry and TTL, build the
// hot_archive_restores / live_restores maps the C++ post-pass uses to
// reclassify CREATED → RESTORED, and fold restored entries / new TTLs
// into the cluster-local accumulator. Live-bucket restores skip both the
// data tx_change and the cluster_local_writes insert (the data/code
// entry itself is unchanged); only the TTL bump folds back.
fn fold_restored_entries(
    output: &RestoreFootprintOutput,
    owned_sources: &[(LedgerKey, LedgerEntry, bool)],
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &mut AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
) -> Result<
    (
        Vec<crate::LedgerEntryDelta>,
        Vec<LedgerEntryUpdate>,
        Vec<LedgerEntryUpdate>,
    ),
    Box<dyn std::error::Error>,
> {
    // Restoration order in the slots vec mirrors the order of writes
    // pushed by restore_footprint_old_env; index `i` lines up
    // restored_entries[i] / new_ttl_entries[i] / owned_sources[i].
    let mut tx_changes: Vec<crate::LedgerEntryDelta> =
        Vec::with_capacity(output.restored_entries.len() + output.new_ttl_entries.len());
    let mut hot_archive_restores: Vec<LedgerEntryUpdate> = Vec::new();
    let mut live_restores: Vec<LedgerEntryUpdate> = Vec::new();
    for (idx, entry) in output.restored_entries.iter().enumerate() {
        let key = ledger_entry_key(entry);
        let (source_key, source_entry, is_archived) = &owned_sources[idx];
        debug_assert_eq!(*source_key, key);
        // Record the restore source for this data/code key. For
        // hot-archive restores the entry value is the archived value;
        // for live restores it's the unchanged live value. The C++
        // post-processor compares this against the new entry to decide
        // RESTORED vs RESTORED+UPDATED.
        let restore_update = LedgerEntryUpdate {
            key_xdr: RustBuf::from(key.to_xdr(Limits::none())?),
            value_xdr: RustBuf::from(source_entry.to_xdr(Limits::none())?),
        };
        if *is_archived {
            hot_archive_restores.push(restore_update);
            // Hot-archive restore: the data/code entry is being brought
            // back into live state, so emit it as a tx_change
            // (CREATED → RESTORED post-processing) and fold into
            // cluster_local_writes for downstream bucket writeback /
            // SorobanState mutation.
            tx_changes.push(build_tx_delta(
                state,
                cross_stage_writes,
                cluster_local_writes,
                classic_prefetch,
                &key,
                Some(entry),
            )?);
            cluster_local_writes.insert(key, Some(entry.clone()));
        } else {
            live_restores.push(restore_update);
            // Live-bucket restore: the data/code entry stays unchanged
            // in live state — only the TTL gets bumped. Skip both the
            // tx_change emission AND the cluster_local_writes insert so
            // bucket writeback doesn't rewrite the unchanged entry, and
            // so the per-op meta doesn't end up with a spurious
            // STATE+UPDATED for it. processOpLedgerEntryChanges injects
            // the LEDGER_ENTRY_RESTORED for this key from the
            // live_restores map at the C++ post-processor step.
        }
    }
    for (idx, ttl_entry) in output.new_ttl_entries.iter().enumerate() {
        let key = ledger_entry_key(ttl_entry);
        let (_, _, is_archived) = &owned_sources[idx];
        // Record the new TTL entry under its key for the restore-source
        // map. For live restores there *was* a (now-expired) TTL in
        // live state, but processOpLedgerEntryChanges only consults the
        // new value here, so storing the new TTL is correct in both
        // cases.
        let restore_update = LedgerEntryUpdate {
            key_xdr: RustBuf::from(key.to_xdr(Limits::none())?),
            value_xdr: RustBuf::from(ttl_entry.to_xdr(Limits::none())?),
        };
        if *is_archived {
            hot_archive_restores.push(restore_update);
        } else {
            live_restores.push(restore_update);
        }
        tx_changes.push(build_tx_delta(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            &key,
            Some(ttl_entry),
        )?);
        cluster_local_writes.insert(key, Some(ttl_entry.clone()));
    }
    Ok((tx_changes, hot_archive_restores, live_restores))
}

