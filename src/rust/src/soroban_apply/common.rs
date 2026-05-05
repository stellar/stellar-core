// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Shared apply-phase utilities used by both the canonical state module
//! and the per-op drivers (invoke / extend / restore / orchestrator).
//!
//! Nothing in here is exposed outside `soroban_apply`; all items are
//! `pub(super)` so siblings of `common` (the other apply submodules) can
//! reach them through the parent module.

use std::borrow::Cow;

use sha2::{Digest, Sha256};

// Non-crypto hashers for the apply-phase HashMaps. LedgerKey hashes and
// TtlKeyHash ([u8;32]) keys are already SHA-256-derived; SipHash's
// avalanche guarantees are wasted CPU. FxHash is deterministic and fast
// for short keys, with a minimal-dep tree.
pub(super) type FastMap<K, V> = rustc_hash::FxHashMap<K, V>;
pub(super) type FastSet<K> = rustc_hash::FxHashSet<K>;
pub(super) use rustc_hash::FxBuildHasher;

use super::state::{EntryRef, SorobanState};
use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::{
    AccountId, FeeBumpTransactionInnerTx, Hash, LedgerEntry, LedgerEntryData, LedgerKey,
    LedgerKeyAccount, LedgerKeyClaimableBalance, LedgerKeyContractCode,
    LedgerKeyContractData, LedgerKeyData, LedgerKeyLiquidityPool, LedgerKeyOffer,
    LedgerKeyTrustLine, LedgerKeyTtl, Limits, Memo, MuxedAccount, Operation, PublicKey,
    ReadXdr, SorobanTransactionData, Transaction, TransactionEnvelope, TransactionExt,
    WriteXdr,
};
use crate::{
    CxxBuf, CxxRentFeeConfiguration, LedgerEntryInput, RustBuf,
};

// SHA-256-derived TTL key hash — the same 32-byte index the C++ side uses to
// key both ContractData/ContractCode entries and their associated TTL.
pub(super) type TtlKeyHash = [u8; 32];

// Protocol from which the in-memory state-size accounting includes the parsed
// WASM module memory cost. Older protocols use only xdr_size for code-rent
// sizing. Mirrors C++ ProtocolVersion::V_23.
pub(super) const STATE_SIZE_ACCOUNTING_PROTOCOL_VERSION: u32 = 23;

// Re-derive a LedgerKey from a stored LedgerEntry. Mirrors the C++
// `LedgerEntryKey(LedgerEntry)`. Covers the Soroban entry types
// (CONTRACT_DATA / CONTRACT_CODE / TTL) and the classic entry types
// the Soroban host can emit as side effects of asset operations
// (Account, Trustline, ClaimableBalance, LiquidityPool, Offer, Data).
// Panics on entry variants outside this set since they shouldn't appear
// in any path the Soroban apply phase touches.
pub(super) fn ledger_entry_key(entry: &LedgerEntry) -> LedgerKey {
    match &entry.data {
        LedgerEntryData::Account(a) => LedgerKey::Account(LedgerKeyAccount {
            account_id: a.account_id.clone(),
        }),
        LedgerEntryData::Trustline(tl) => LedgerKey::Trustline(LedgerKeyTrustLine {
            account_id: tl.account_id.clone(),
            asset: tl.asset.clone(),
        }),
        LedgerEntryData::ContractData(d) => LedgerKey::ContractData(LedgerKeyContractData {
            contract: d.contract.clone(),
            key: d.key.clone(),
            durability: d.durability,
        }),
        LedgerEntryData::ContractCode(c) => {
            LedgerKey::ContractCode(LedgerKeyContractCode { hash: c.hash.clone() })
        }
        LedgerEntryData::Ttl(t) => {
            LedgerKey::Ttl(LedgerKeyTtl { key_hash: t.key_hash.clone() })
        }
        LedgerEntryData::ClaimableBalance(cb) => {
            LedgerKey::ClaimableBalance(LedgerKeyClaimableBalance {
                balance_id: cb.balance_id.clone(),
            })
        }
        LedgerEntryData::LiquidityPool(lp) => LedgerKey::LiquidityPool(LedgerKeyLiquidityPool {
            liquidity_pool_id: lp.liquidity_pool_id.clone(),
        }),
        LedgerEntryData::Offer(o) => LedgerKey::Offer(LedgerKeyOffer {
            seller_id: o.seller_id.clone(),
            offer_id: o.offer_id,
        }),
        LedgerEntryData::Data(d) => LedgerKey::Data(LedgerKeyData {
            account_id: d.account_id.clone(),
            data_name: d.data_name.clone(),
        }),
        _ => panic!("ledger_entry_key called with unsupported entry type"),
    }
}

// Compute the TTL key hash for a CONTRACT_DATA / CONTRACT_CODE / TTL key.
// Mirrors C++ `getTTLKey(LedgerKey).ttl().keyHash`:
// - For TTL keys, returns the embedded `key_hash` directly.
// - For CONTRACT_DATA / CONTRACT_CODE keys, returns SHA256 of the
//   XDR-serialized LedgerKey (including the discriminator), matching the
//   C++ `xdrSha256` helper's behaviour.
pub(super) fn ttl_key_hash_for(key: &LedgerKey) -> TtlKeyHash {
    if let LedgerKey::Ttl(ttl_key) = key {
        return ttl_key.key_hash.0;
    }
    let serialized = key
        .to_xdr(Limits::none())
        .expect("XDR serialize of LedgerKey cannot fail at finite-size limits");
    Sha256::digest(&serialized).into()
}

// Convenience: derive the TTL LedgerKey for a CONTRACT_DATA / CONTRACT_CODE
// LedgerKey. Panics for any other input — callers should only ask for TTL
// keys of Soroban entries.
pub(super) fn ttl_lookup_key_for(key: &LedgerKey) -> LedgerKey {
    LedgerKey::Ttl(LedgerKeyTtl {
        key_hash: Hash(ttl_key_hash_for(key)),
    })
}

// Extract `live_until_ledger_seq` from a TTL LedgerEntry. Returns None for
// non-TTL entries.
pub(super) fn ttl_live_until_of(entry: &LedgerEntry) -> Option<u32> {
    match &entry.data {
        LedgerEntryData::Ttl(t) => Some(t.live_until_ledger_seq),
        _ => None,
    }
}

// Extract `live_until_ledger_seq` from the TTL LedgerEntry stored in
// `writes[key]`. Returns None when the key is absent, mapped to a deletion,
// or holds a non-TTL entry.
pub(super) fn ttl_live_until_in_writes(
    writes: &AccumulatedWrites,
    key: &LedgerKey,
) -> Option<u32> {
    match writes.get(key) {
        Some(Some(le)) => ttl_live_until_of(le),
        _ => None,
    }
}

// Max-merge a new TTL `LedgerEntry` for `ttl_key` into `writes`: if `writes`
// already holds a TTL with `live_until_ledger_seq` >= the incoming entry's,
// keep it. Otherwise overwrite with `new_ttl`. Returns true when the new
// entry won.
pub(super) fn merge_ttl_max(
    writes: &mut AccumulatedWrites,
    ttl_key: LedgerKey,
    new_ttl: LedgerEntry,
) -> bool {
    let incoming = match ttl_live_until_of(&new_ttl) {
        Some(v) => v,
        None => return false,
    };
    if let Some(existing) = ttl_live_until_in_writes(writes, &ttl_key) {
        if existing >= incoming {
            return false;
        }
    }
    writes.insert(ttl_key, Some(new_ttl));
    true
}

// XDR-serialized size of a LedgerEntry, matching C++ `xdr::xdr_size(entry)`.
// Used as the cached `size_bytes` for CONTRACT_DATA entries (and for
// CONTRACT_CODE entries on protocols pre-23, where in-memory module size
// isn't yet part of the rent-fee accounting).
pub(super) fn xdr_serialized_size(entry: &LedgerEntry) -> u32 {
    let bytes = entry
        .to_xdr(Limits::none())
        .expect("XDR serialize of LedgerEntry cannot fail at finite-size limits");
    u32::try_from(bytes.len()).expect("LedgerEntry XDR size exceeds u32")
}

// Compute the rent-fee size for a CONTRACT_CODE LedgerEntry. Mirrors C++
// `contractCodeSizeForRent` + `ledgerEntrySizeForRent`: always uses
// protocol >= 23 for the size compute (so the size is "correct on
// upgrade-to-23 boundary"). On protocols >= 23, adds the parsed-WASM
// in-memory module size from soroban-env-host's wasm_module_memory_cost.
pub(super) fn compute_contract_code_size_for_rent(
    entry: &LedgerEntry,
    config_max_protocol: u32,
    ledger_version: u32,
    cpu_cost_params: &[u8],
    mem_cost_params: &[u8],
) -> u32 {
    let xdr_size = xdr_serialized_size(entry);
    let cc = match &entry.data {
        LedgerEntryData::ContractCode(c) => c,
        _ => panic!(
            "compute_contract_code_size_for_rent: non-CONTRACT_CODE LedgerEntry"
        ),
    };
    let cc_xdr = cc
        .to_xdr(Limits::none())
        .expect("compute_contract_code_size_for_rent: serialize ContractCodeEntry");
    let version_for_size = ledger_version.max(STATE_SIZE_ACCOUNTING_PROTOCOL_VERSION);
    let memory_size = crate::soroban_module_cache::contract_code_memory_size_for_rent_bytes(
        config_max_protocol,
        version_for_size,
        &cc_xdr,
        cpu_cost_params,
        mem_cost_params,
    )
    .expect("contract_code_memory_size_for_rent_bytes");
    let total = u64::from(xdr_size).saturating_add(u64::from(memory_size));
    u32::try_from(total.min(u64::from(u32::MAX))).unwrap_or(u32::MAX)
}

// Patch the first 4 bytes of an encoded `LedgerEntry` so its
// `lastModifiedLedgerSeq` field reflects `seq` instead of whatever the
// host wrote. The host doesn't bump this field on its own (the legacy
// C++ path bumped it inside LedgerTxn::maybeUpdateLastModified); we
// need the bumped value in the bytes that go to bucket writeback. The
// XDR layout for `LedgerEntry` puts `lastModifiedLedgerSeq: uint32` at
// offset 0 in big-endian, so a 4-byte memcpy is enough.
#[inline]
pub(super) fn patch_last_modified_seq(bytes: &mut [u8], seq: u32) {
    if bytes.len() >= 4 {
        bytes[..4].copy_from_slice(&seq.to_be_bytes());
    }
}

// CxxRentFeeConfiguration is a cxx shared struct that doesn't derive Clone
// (cxx's default for shared structs). All fields are i64 primitives, so we
// just rebuild it manually.
pub(super) fn copy_rent_fee_config(c: &CxxRentFeeConfiguration) -> CxxRentFeeConfiguration {
    CxxRentFeeConfiguration {
        fee_per_write_1kb: c.fee_per_write_1kb,
        fee_per_rent_1kb: c.fee_per_rent_1kb,
        fee_per_write_entry: c.fee_per_write_entry,
        persistent_rent_rate_denominator: c.persistent_rent_rate_denominator,
        temporary_rent_rate_denominator: c.temporary_rent_rate_denominator,
    }
}

// Same shape as `copy_rent_fee_config` for the full `CxxFeeConfiguration`.
pub(super) fn copy_fee_config(
    c: &crate::CxxFeeConfiguration,
) -> crate::CxxFeeConfiguration {
    crate::CxxFeeConfiguration {
        fee_per_instruction_increment: c.fee_per_instruction_increment,
        fee_per_disk_read_entry: c.fee_per_disk_read_entry,
        fee_per_write_entry: c.fee_per_write_entry,
        fee_per_disk_read_1kb: c.fee_per_disk_read_1kb,
        fee_per_write_1kb: c.fee_per_write_1kb,
        fee_per_historical_1kb: c.fee_per_historical_1kb,
        fee_per_contract_event_1kb: c.fee_per_contract_event_1kb,
        fee_per_transaction_size_1kb: c.fee_per_transaction_size_1kb,
    }
}

// Compute the events portion of the resource fee — the value the
// C++ post-pass would otherwise back-call into Rust to recompute via
// `RefundableFeeTracker::consumeRefundableSorobanResources`'s inner
// `computeSorobanResourceFee` call. Mirrors the C++ side's
// CxxTransactionResources construction; result is the
// `refundable_fee` field of the host's
// `compute_transaction_resource_fee` output.
pub(super) fn compute_refundable_fee_increment(
    config_max_protocol: u32,
    protocol_version: u32,
    resources: &crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::SorobanResources,
    archived_soroban_entries_count: u32,
    is_restore_footprint_op: bool,
    transaction_size_bytes: u32,
    contract_events_size_bytes: u32,
    fee_config: crate::CxxFeeConfiguration,
) -> Result<i64, Box<dyn std::error::Error>> {
    let disk_read_entries = if is_restore_footprint_op {
        resources.footprint.read_write.len() as u32
    } else {
        // Mirrors `getNumDiskReadEntries` for V_23+: count classic
        // (non-Soroban) entries + archivedSorobanEntries from
        // resourceExt v1.
        let mut count: u32 = 0;
        for k in resources
            .footprint
            .read_only
            .iter()
            .chain(resources.footprint.read_write.iter())
        {
            let is_soroban = matches!(
                k,
                crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::LedgerKey::ContractData(_)
                    | crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::LedgerKey::ContractCode(_)
                    | crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::LedgerKey::Ttl(_)
            );
            if !is_soroban {
                count += 1;
            }
        }
        count += archived_soroban_entries_count;
        count
    };

    let cxx_resources = crate::CxxTransactionResources {
        instructions: resources.instructions,
        disk_read_entries,
        write_entries: resources.footprint.read_write.len() as u32,
        disk_read_bytes: resources.disk_read_bytes,
        write_bytes: resources.write_bytes,
        transaction_size_bytes,
        contract_events_size_bytes,
    };
    let pair = crate::soroban_invoke::compute_transaction_resource_fee(
        config_max_protocol,
        protocol_version,
        cxx_resources,
        fee_config,
    )?;
    Ok(pair.refundable_fee)
}

// CxxBuf-construction helpers used by the bytes-path host call.
pub(super) fn bytes_to_cxx_buf(bytes: &[u8]) -> CxxBuf {
    CxxBuf {
        data: unsafe {
            crate::rust_bridge::shim_copyU8Vector(bytes.as_ptr(), bytes.len())
        },
    }
}

pub(super) fn xdr_to_cxx_buf<T: WriteXdr>(value: &T) -> CxxBuf {
    let bytes = value
        .to_xdr(Limits::none())
        .expect("XDR serialize cannot fail at finite-size limits");
    bytes_to_cxx_buf(&bytes)
}

// Convert a `MuxedAccount` to its underlying `AccountId`. Mirrors the
// implicit conversion the legacy bytes path relied on (the host
// `metered_from_xdr`'d the source account buf as `AccountId` even when
// the C++ side handed it a MuxedAccount; that worked for Ed25519 because
// both XDR shapes start with `u32(0) + 32 bytes`, but it breaks for
// MuxedEd25519). The typed path needs an explicit `AccountId`, so we do
// the conversion at the boundary and use the underlying ed25519 key for
// both Ed25519 and MuxedEd25519 source accounts.
//
// By-move variant: the typed host call consumes the source_account, so
// we destructure to avoid cloning the 32-byte ed25519 key the bytes-
// variant has to clone.
pub(super) fn muxed_to_account_id_owned(m: MuxedAccount) -> AccountId {
    let pk = match m {
        MuxedAccount::Ed25519(k) => PublicKey::PublicKeyTypeEd25519(k),
        MuxedAccount::MuxedEd25519(med) => PublicKey::PublicKeyTypeEd25519(med.ed25519),
    };
    AccountId(pk)
}

// Per-phase write accumulator. Keyed by LedgerKey directly so a CONTRACT_DATA
// modification and its associated TTL modification stay distinct (they have
// different LedgerKey shapes — `LedgerKey::ContractData(...)` vs
// `LedgerKey::Ttl(...)`). `None` represents a delete/tombstone.
pub(super) type AccumulatedWrites = FastMap<LedgerKey, Option<LedgerEntry>>;

// Reasons a per-TX driver can return a failed SorobanTxApplyResult before
// reaching the host. Used to populate the matching `is_*` flag without
// having to write the full empty result struct at each call site.
pub(super) enum SorobanTxFailure {
    EntryArchived,
    ResourceLimitExceeded,
    InsufficientRefundableFee,
}

// Build a failed SorobanTxApplyResult that drops every per-TX output (writes,
// events, deltas, fees) and surfaces the matching `is_*` flag. Mirrors the
// "empty failure result" pattern every per-TX driver hits when it bails out
// before calling the host (archived entry, cap exceeded, etc.).
pub(super) fn make_tx_failure_result(
    failure: SorobanTxFailure,
    diagnostic_events: Vec<crate::RustBuf>,
) -> crate::SorobanTxApplyResult {
    use SorobanTxFailure::*;
    crate::SorobanTxApplyResult {
        success: false,
        is_internal_error: false,
        is_insufficient_refundable_fee: matches!(failure, InsufficientRefundableFee),
        is_resource_limit_exceeded: matches!(failure, ResourceLimitExceeded),
        is_entry_archived: matches!(failure, EntryArchived),
        return_value_xdr: crate::RustBuf::from(Vec::<u8>::new()),
        contract_events: Vec::new(),
        diagnostic_events,
        rent_fee_consumed: 0,
        contract_event_size_bytes: 0,
        tx_changes: Vec::new(),
        hot_archive_restores: Vec::new(),
        live_restores: Vec::new(),
        success_preimage_hash: crate::RustBuf::from(Vec::<u8>::new()),
        refundable_fee_increment: 0,
    }
}

// Layered read of a single LedgerEntry during the phase. Priority order:
//   1. cluster_local (per-cluster writes accumulated by this worker)
//   2. cross_stage (writes from earlier stages in this phase)
//   3. SorobanState (canonical, for CONTRACT_DATA / CONTRACT_CODE / TTL)
//   4. classic_prefetch (for non-Soroban entries the Soroban TXs need —
//      typically just the source account)
//
// The per-cluster layer is what makes parallel cluster execution sound:
// each worker reads its own writes plus the previous-stage cross-stage
// snapshot, but never sees writes from a sibling cluster running in
// parallel in the same stage. The orchestrator merges per-cluster locals
// into cross_stage at the stage barrier.
//
// Returns a `Cow<LedgerEntry>` so callers that only need to read can borrow
// straight from the source layer (state, cluster_local, cross_stage,
// classic_prefetch) without paying for a clone. `Cow::into_owned()` is
// the explicit conversion when a caller needs to keep / mutate / hand
// ownership downstream (e.g. push into cluster_local_writes).
//
// Note: state.get() can synthesize a fresh LedgerEntry for TTL lookups
// (TtlData is stored inline), so `EntryRef::Owned` becomes
// `Cow::Owned`; everything else borrows.
pub(super) fn layered_get<'a>(
    state: &'a SorobanState,
    cross_stage: &'a AccumulatedWrites,
    cluster_local: &'a AccumulatedWrites,
    classic_prefetch: &'a FastMap<LedgerKey, LedgerEntry>,
    key: &LedgerKey,
) -> Option<Cow<'a, LedgerEntry>> {
    if let Some(slot) = cluster_local.get(key) {
        return slot.as_ref().map(Cow::Borrowed);
    }
    if let Some(slot) = cross_stage.get(key) {
        return slot.as_ref().map(Cow::Borrowed);
    }
    match key {
        LedgerKey::ContractData(_) | LedgerKey::ContractCode(_) | LedgerKey::Ttl(_) => {
            state.get(key).map(|er| match er {
                EntryRef::Borrowed(e) => Cow::Borrowed(e),
                EntryRef::Owned(e) => Cow::Owned(e),
            })
        }
        _ => classic_prefetch.get(key).map(Cow::Borrowed),
    }
}

// Build a single LedgerEntryDelta capturing the prev value (via
// layered_get against the layered state at this point in cluster
// execution) for the given key, paired with the new entry the TX is
// about to write. Drivers must call this BEFORE folding the new entry
// into cluster_local_writes — once the new value is in
// cluster_local_writes, layered_get would return the new value as
// "prev", which is wrong.
pub(super) fn build_tx_delta(
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    key: &LedgerKey,
    new_entry: Option<&LedgerEntry>,
) -> Result<crate::LedgerEntryDelta, Box<dyn std::error::Error>> {
    build_tx_delta_with_cached_new(
        state,
        cross_stage_writes,
        cluster_local_writes,
        classic_prefetch,
        key,
        new_entry,
        None,
    )
}

// Same as `build_tx_delta` but lets the caller hand in already-encoded
// bytes for `new_entry`. The host's typed-output path (V_26+) returns
// each modified entry alongside its `metered_write_xdr` bytes; we keep
// those bytes around so the per-TX delta and the phase-end ledger
// update can both reuse them, skipping the extra `to_xdr` pass.
pub(super) fn build_tx_delta_with_cached_new(
    state: &SorobanState,
    cross_stage_writes: &AccumulatedWrites,
    cluster_local_writes: &AccumulatedWrites,
    classic_prefetch: &FastMap<LedgerKey, LedgerEntry>,
    key: &LedgerKey,
    new_entry: Option<&LedgerEntry>,
    cached_new_bytes: Option<&[u8]>,
) -> Result<crate::LedgerEntryDelta, Box<dyn std::error::Error>> {
    let prev = layered_get(
        state,
        cross_stage_writes,
        cluster_local_writes,
        classic_prefetch,
        key,
    );
    let key_bytes = key.to_xdr(Limits::none())?;
    let prev_bytes = match &prev {
        Some(e) => e.to_xdr(Limits::none())?,
        None => Vec::<u8>::new(),
    };
    let new_bytes = match (new_entry, cached_new_bytes) {
        (Some(_), Some(cached)) => cached.to_vec(),
        (Some(e), None) => e.to_xdr(Limits::none())?,
        (None, _) => Vec::<u8>::new(),
    };
    Ok(crate::LedgerEntryDelta {
        key_xdr: RustBuf::from(key_bytes),
        prev_value_xdr: RustBuf::from(prev_bytes),
        new_value_xdr: RustBuf::from(new_bytes),
    })
}

// Build a FastMap<LedgerKey, LedgerEntry> from a Vec<LedgerEntryInput>
// for fast layered_get lookups. Skips entries with empty value bytes
// (those represent deletions; never appear on input in practice).
pub(super) fn build_prefetch_map(
    inputs: &[LedgerEntryInput],
) -> Result<FastMap<LedgerKey, LedgerEntry>, Box<dyn std::error::Error>> {
    let mut map =
        FastMap::with_capacity_and_hasher(inputs.len(), FxBuildHasher::default());
    for u in inputs {
        let value_bytes = u.value_xdr.as_ref();
        if value_bytes.is_empty() {
            continue;
        }
        let key = LedgerKey::from_xdr(u.key_xdr.as_ref(), Limits::none())?;
        let entry = LedgerEntry::from_xdr(value_bytes, Limits::none())?;
        map.insert(key, entry);
    }
    Ok(map)
}

// Pull the source_account / operations / SorobanTransactionData out of
// a `TransactionEnvelope` by-move so the caller can destructure them
// downstream without paying for a clone of large fields (host_function,
// auth, resources). For Soroban TXs the envelope is always TxOrFeeBump
// (not TxV0) and the inner tx always carries V1 SorobanTransactionData,
// so we can destructure without information loss.
pub(super) fn extract_tx_parts_owned(
    envelope: TransactionEnvelope,
) -> Result<
    (MuxedAccount, Vec<Operation>, SorobanTransactionData),
    Box<dyn std::error::Error>,
> {
    let tx: Transaction = match envelope {
        TransactionEnvelope::Tx(e) => e.tx,
        TransactionEnvelope::TxFeeBump(e) => match e.tx.inner_tx {
            FeeBumpTransactionInnerTx::Tx(inner) => inner.tx,
        },
        TransactionEnvelope::TxV0(_) => {
            return Err(
                "extract_tx_parts_owned: Soroban TX cannot be a TxV0 envelope"
                    .into(),
            );
        }
    };
    let Transaction { source_account, operations, ext, .. } = tx;
    let soroban_data = match ext {
        TransactionExt::V1(d) => d,
        TransactionExt::V0 => {
            return Err(
                "extract_tx_parts_owned: Soroban TX must have TransactionExt::V1(SorobanTransactionData)".into(),
            );
        }
    };
    Ok((source_account, operations.into(), soroban_data))
}

// Borrowing variant of `extract_tx_parts_owned`. Used by callers that
// want to inspect the envelope without consuming it (e.g. memo check).
pub(super) fn extract_tx_parts<'a>(
    envelope: &'a TransactionEnvelope,
) -> Result<
    (&'a MuxedAccount, &'a [Operation], &'a SorobanTransactionData),
    Box<dyn std::error::Error>,
> {
    let tx: &Transaction = match envelope {
        TransactionEnvelope::Tx(e) => &e.tx,
        TransactionEnvelope::TxFeeBump(e) => match &e.tx.inner_tx {
            FeeBumpTransactionInnerTx::Tx(inner) => &inner.tx,
        },
        TransactionEnvelope::TxV0(_) => {
            return Err(
                "extract_tx_parts: Soroban TX cannot be a TxV0 envelope".into(),
            );
        }
    };
    let soroban_data = match &tx.ext {
        TransactionExt::V1(d) => d,
        TransactionExt::V0 => {
            return Err(
                "extract_tx_parts: Soroban TX must have TransactionExt::V1(SorobanTransactionData)".into(),
            );
        }
    };
    Ok((&tx.source_account, tx.operations.as_slice(), soroban_data))
}

// Mirrors the BUILD_TESTS-only maybeTriggerTestInternalError hook in
// TransactionFrame.cpp: a TX whose Memo is the literal text
// "txINTERNAL_ERROR" is meant to fail with txINTERNAL_ERROR. The new
// orchestrator detects the memo BEFORE the host runs and skips
// dispatch entirely so writes don't pollute cluster_local_writes /
// ro_ttl_bumps.
pub(super) fn has_test_internal_error_memo(envelope: &TransactionEnvelope) -> bool {
    let memo = match envelope {
        TransactionEnvelope::Tx(e) => &e.tx.memo,
        TransactionEnvelope::TxFeeBump(e) => match &e.tx.inner_tx {
            FeeBumpTransactionInnerTx::Tx(inner) => &inner.tx.memo,
        },
        TransactionEnvelope::TxV0(e) => &e.tx.memo,
    };
    if let Memo::Text(s) = memo {
        s.as_vec().as_slice() == b"txINTERNAL_ERROR"
    } else {
        false
    }
}

// Per-TX PRNG seed derivation. Mirrors the C++ `subSha256(seed, tx_num)`
// helper: SHA256(seed || xdr_to_opaque(tx_num)). xdr_to_opaque on a u64 is
// 8 bytes big-endian (XDR encoding for unsigned hyper).
pub(super) fn derive_per_tx_prng_seed(base_seed: &[u8], tx_num: u64) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(base_seed);
    hasher.update(&tx_num.to_be_bytes());
    hasher.finalize().into()
}
