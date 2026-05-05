// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! ExtendFootprintTtl op driver. Bumps the TTL of each footprint slot
//! whose current TTL is below the requested live-until ledger, and
//! computes the rent fee owed for the bump via the protocol-pinned
//! soroban-env-host's rent calculator.

use std::borrow::Cow;

use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::{
    ContractDataDurability, ExtendFootprintTtlOp, LedgerEntry, LedgerEntryData, LedgerKey,
    SorobanResources,
};

use super::common::{
    build_tx_delta, compute_contract_code_size_for_rent, layered_get, ledger_entry_key,
    make_tx_failure_result, ttl_live_until_of, ttl_lookup_key_for, xdr_serialized_size,
    AccumulatedWrites, FastMap, SorobanTxFailure,
};
use super::state::SorobanState;
use crate::{CxxLedgerEntryRentChange, CxxLedgerInfo, CxxRentFeeConfiguration, RustBuf};

struct ExtendFootprintTtlSlot<'a> {
    ledger_entry: &'a LedgerEntry, // CONTRACT_DATA / CONTRACT_CODE
    ttl_entry: &'a LedgerEntry,    // current TTL LedgerEntry
}

struct ExtendFootprintTtlOutput {
    // Updated TTL LedgerEntries to write back. Slots whose current TTL
    // already exceeds the requested new TTL are silently dropped (no-op
    // rent-wise, mirroring the C++ "currLiveUntilLedgerSeq >=
    // newLiveUntilLedgerSeq" early-skip).
    modified_ttl_entries: Vec<LedgerEntry>,
    rent_fee: i64,
}

fn extend_footprint_ttl_old_env(
    config_max_protocol: u32,
    protocol_version: u32,
    current_ledger_seq: u32,
    extend_to: u32,
    rent_fee_configuration: CxxRentFeeConfiguration,
    cpu_cost_params: &[u8],
    mem_cost_params: &[u8],
    slots: &[ExtendFootprintTtlSlot<'_>],
) -> Result<ExtendFootprintTtlOutput, Box<dyn std::error::Error>> {
    // C++ extends "for `extendTo` more ledgers" relative to the current
    // ledger; the current ledger itself has to be paid for already.
    let new_live_until = current_ledger_seq.saturating_add(extend_to);

    let mut modified_ttl_entries = Vec::new();
    let mut rent_changes: Vec<CxxLedgerEntryRentChange> = Vec::new();

    for slot in slots {
        let LedgerEntryData::Ttl(ttl) = &slot.ttl_entry.data else {
            return Err("extend_footprint_ttl_old_env: ttl_entry is not TTL".into());
        };
        let current_live_until = ttl.live_until_ledger_seq;
        if current_live_until >= new_live_until {
            // No-op: current TTL already covers the requested range.
            continue;
        }

        let (is_persistent, is_code_entry) = match &slot.ledger_entry.data {
            LedgerEntryData::ContractData(d) => (
                d.durability == ContractDataDurability::Persistent,
                false,
            ),
            LedgerEntryData::ContractCode(_) => (true, true),
            _ => {
                return Err(
                    "extend_footprint_ttl_old_env: ledger_entry is not CONTRACT_DATA or CONTRACT_CODE"
                        .into(),
                );
            }
        };
        // Rent-aware size: for protocol >= 23 + CONTRACT_CODE, this is
        // xdr_size + parsed-module memory footprint; otherwise it's
        // plain xdr_size. Mirrors C++ `ledgerEntrySizeForRent`.
        let entry_size = if is_code_entry {
            compute_contract_code_size_for_rent(
                slot.ledger_entry,
                config_max_protocol,
                protocol_version,
                cpu_cost_params,
                mem_cost_params,
            )
        } else {
            xdr_serialized_size(slot.ledger_entry)
        };

        rent_changes.push(CxxLedgerEntryRentChange {
            is_persistent,
            is_code_entry,
            old_size_bytes: entry_size,
            new_size_bytes: entry_size,
            old_live_until_ledger: current_live_until,
            new_live_until_ledger: new_live_until,
        });

        // Construct the bumped TTL LedgerEntry. lastModifiedLedgerSeq updates
        // to the current ledger to match the C++ ltx behavior on upsert.
        let mut new_ttl = slot.ttl_entry.clone();
        new_ttl.last_modified_ledger_seq = current_ledger_seq;
        if let LedgerEntryData::Ttl(t) = &mut new_ttl.data {
            t.live_until_ledger_seq = new_live_until;
        }
        modified_ttl_entries.push(new_ttl);
    }

    let rent_fee = crate::soroban_invoke::compute_rent_fee(
        config_max_protocol,
        protocol_version,
        &rent_changes,
        rent_fee_configuration,
        current_ledger_seq,
    )?;

    Ok(ExtendFootprintTtlOutput {
        modified_ttl_entries,
        rent_fee,
    })
}

// Per-TX driver for the ExtendFootprintTtl op: walks the RO footprint,
// resolves each entry's live TTL through the layered state, runs the
// rent-fee adapter, and buffers the bumped TTL entries in ro_ttl_bumps
// for the orchestrator to drain at cluster end. Mirrors the legacy C++
// ExtendFootprintTTLApplyHelper flow.
pub(super) fn apply_extend_footprint_ttl(
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &mut AccumulatedWrites,
    ro_ttl_bumps: &mut FastMap<LedgerKey, (LedgerEntry, Vec<u8>)>,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    config_max_protocol: u32,
    ledger_info: &CxxLedgerInfo,
    rent_fee_configuration: CxxRentFeeConfiguration,
    op: &ExtendFootprintTtlOp,
    resources: &SorobanResources,
    max_refundable_fee: i64,
) -> Result<crate::SorobanTxApplyResult, Box<dyn std::error::Error>> {
    let current_ledger_seq = ledger_info.sequence_number;
    // Pre-resolve each read-only footprint slot. Slots that fail any check
    // are silently skipped — matches the C++ "extend as many entries as
    // possible" behaviour.
    //
    // Hold each slot's data + ttl as a `Cow<LedgerEntry>` so a state-
    // sourced entry stays borrowed all the way down into the slots
    // ExtendFootprintTtl hands to the rent-fee compute. Only the
    // (necessarily owned) TTL synthesized by `state.get(Ttl(_))`
    // — and any value-type entries the layered layers happen to own
    // — incur a clone, and even then only into the same Vec we'd need
    // anyway to keep the slot refs alive.
    let mut data_entries: Vec<Cow<'_, LedgerEntry>> = Vec::new();
    let mut ttl_entries: Vec<Cow<'_, LedgerEntry>> = Vec::new();
    for k in resources.footprint.read_only.iter() {
        if !matches!(
            k,
            LedgerKey::ContractData(_) | LedgerKey::ContractCode(_)
        ) {
            continue;
        }
        let Some(data_entry) = layered_get(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            k,
        ) else {
            continue;
        };
        // Per-entry size cap — mirrors legacy validateContractLedgerEntry.
        // ExtendFootprintTtl on an over-limit entry fails the whole op with
        // EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED.
        let entry_size = xdr_serialized_size(&data_entry);
        let cap_exceeded = match &data_entry.data {
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
        let ttl_key = ttl_lookup_key_for(k);
        let Some(ttl_entry) = layered_get(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            &ttl_key,
        ) else {
            continue;
        };
        // Skip expired entries — the C++ side requires entries to be live
        // before they can be extended.
        let LedgerEntryData::Ttl(ttl) = &ttl_entry.data else {
            continue;
        };
        if ttl.live_until_ledger_seq < current_ledger_seq {
            continue;
        }
        data_entries.push(data_entry);
        ttl_entries.push(ttl_entry);
    }

    let slots: Vec<ExtendFootprintTtlSlot<'_>> = data_entries
        .iter()
        .zip(ttl_entries.iter())
        .map(|(d, t)| ExtendFootprintTtlSlot {
            ledger_entry: d.as_ref(),
            ttl_entry: t.as_ref(),
        })
        .collect();

    let output = extend_footprint_ttl_old_env(
        config_max_protocol,
        ledger_info.protocol_version,
        current_ledger_seq,
        op.extend_to,
        rent_fee_configuration,
        ledger_info.cpu_cost_params.as_ref(),
        ledger_info.mem_cost_params.as_ref(),
        &slots,
    )?;

    // Refundable-fee budget check — mirrors the legacy
    // ExtendFootprintTTLOpFrame consumeRefundableResources path: if the
    // computed rent fee exceeds the TX's max refundable budget, fail
    // before folding any writes (so SorobanState / cluster_local_writes
    // stay clean and bucket writeback skips this TX).
    if output.rent_fee > max_refundable_fee {
        return Ok(make_tx_failure_result(
            SorobanTxFailure::InsufficientRefundableFee,
            Vec::new(),
        ));
    }

    // Capture deltas BEFORE folding into the accumulator. ExtendFootprintTtl
    // mutates only TTL entries — the underlying data/code rows are untouched.
    // ExtendFootprintTtl always operates on RO footprint keys, so the TTL
    // bumps it produces flow into ro_ttl_bumps (buffered, max-merged at
    // cluster end) instead of cluster_local_writes. This matches the
    // legacy ThreadParallelApplyLedgerState::mRoTTLBumps semantics where
    // RO TTL bumps are not visible to sibling RO TXs in the same cluster.
    let tx_changes = fold_extended_ttls(
        output.modified_ttl_entries,
        state,
        cross_stage_writes,
        cluster_local_writes,
        classic_prefetch,
        ro_ttl_bumps,
    )?;

    // ExtendFootprintTtl has no return value or contract events — just
    // the success/failure code and the rent fee paid for the TTL bumps.
    // It also doesn't restore anything (only extends existing TTLs).
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
        hot_archive_restores: Vec::new(),
        live_restores: Vec::new(),
        success_preimage_hash: RustBuf::from(Vec::<u8>::new()),
        refundable_fee_increment: 0,
    })
}

// Capture per-TX deltas for each bumped TTL entry and buffer the bumped
// entries into `ro_ttl_bumps`. Each TTL is max-merged against any prior
// RO bump for the same key. Returns the captured `tx_changes` vec.
fn fold_extended_ttls(
    modified_ttl_entries: Vec<LedgerEntry>,
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    ro_ttl_bumps: &mut FastMap<LedgerKey, (LedgerEntry, Vec<u8>)>,
) -> Result<Vec<crate::LedgerEntryDelta>, Box<dyn std::error::Error>> {
    let mut tx_changes: Vec<crate::LedgerEntryDelta> =
        Vec::with_capacity(modified_ttl_entries.len());
    for ttl_entry in modified_ttl_entries {
        let key = ledger_entry_key(&ttl_entry);
        tx_changes.push(build_tx_delta(
            state,
            cross_stage_writes,
            cluster_local_writes,
            classic_prefetch,
            &key,
            Some(&ttl_entry),
        )?);
        let bumped_live_until = match ttl_live_until_of(&ttl_entry) {
            Some(v) => v,
            None => continue,
        };
        let buffered_higher = ro_ttl_bumps
            .get(&key)
            .and_then(|(le, _)| ttl_live_until_of(le))
            .map(|prev| prev >= bumped_live_until)
            .unwrap_or(false);
        if !buffered_higher {
            // ExtendFootprintTtl only has the typed TTL on hand; leave
            // the bytes empty so the phase-end commit re-encodes from
            // the typed entry. The host-fn invoke path populates real
            // bytes — see invoke.rs.
            ro_ttl_bumps.insert(key, (ttl_entry, Vec::new()));
        }
    }
    Ok(tx_changes)
}
