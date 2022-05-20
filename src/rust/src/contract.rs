// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        Bump, CxxBuf, CxxFeeConfiguration, CxxLedgerEntryRentChange, CxxLedgerInfo,
        CxxRentFeeConfiguration, CxxTransactionResources, CxxWriteFeeConfiguration, FeePair,
        InvokeHostFunctionOutput, RustBuf, XDRFileHash,
    },
};
use log::debug;
use std::{fmt::Display, io::Cursor, panic, rc::Rc};
use tracy_client::{span, Client};

// This module (contract) is bound to _two separate locations_ in the module
// tree: crate::lo::contract and crate::hi::contract, each of which has a (lo or
// hi) version-specific definition of stellar_env_host. We therefore
// import it from our _parent_ module rather than from the crate root.
use super::soroban_env_host::{
    budget::Budget,
    events::Events,
    expiration_ledger_bumps::ExpirationLedgerBumps,
    fees::{
        compute_rent_fee as host_compute_rent_fee,
        compute_transaction_resource_fee as host_compute_transaction_resource_fee,
        compute_write_fee_per_1kb as host_compute_write_fee_per_1kb, FeeConfiguration,
        LedgerEntryRentChange, RentFeeConfiguration, TransactionResources, WriteFeeConfiguration,
    },
    storage::{self, AccessType, Footprint, FootprintMap, Storage, StorageMap},
    xdr::{
        self, AccountId, ContractCodeEntryBody, ContractCostParams, ContractDataEntryBody,
        ContractEntryBodyType, ContractEvent, ContractEventType, DiagnosticEvent, HostFunction,
        LedgerEntry, LedgerEntryData, LedgerKey, LedgerKeyAccount, LedgerKeyContractCode,
        LedgerKeyContractData, LedgerKeyTrustLine, ReadXdr, ScErrorCode, ScErrorType,
        SorobanAuthorizationEntry, SorobanResources, WriteXdr, XDR_FILES_SHA256,
    },
    DiagnosticLevel, Host, HostError, LedgerInfo,
};
use std::error::Error;

impl From<CxxLedgerInfo> for LedgerInfo {
    fn from(c: CxxLedgerInfo) -> Self {
        Self {
            protocol_version: c.protocol_version,
            sequence_number: c.sequence_number,
            timestamp: c.timestamp,
            network_id: c.network_id.try_into().unwrap(),
            base_reserve: c.base_reserve,
            min_temp_entry_expiration: c.min_temp_entry_expiration,
            min_persistent_entry_expiration: c.min_persistent_entry_expiration,
            max_entry_expiration: c.max_entry_expiration,
        }
    }
}

impl From<CxxTransactionResources> for TransactionResources {
    fn from(value: CxxTransactionResources) -> Self {
        Self {
            instructions: value.instructions,
            read_entries: value.read_entries,
            write_entries: value.write_entries,
            read_bytes: value.read_bytes,
            write_bytes: value.write_bytes,
            metadata_size_bytes: value.metadata_size_bytes,
            transaction_size_bytes: value.transaction_size_bytes,
        }
    }
}

impl From<CxxFeeConfiguration> for FeeConfiguration {
    fn from(value: CxxFeeConfiguration) -> Self {
        Self {
            fee_per_instruction_increment: value.fee_per_instruction_increment,
            fee_per_read_entry: value.fee_per_read_entry,
            fee_per_write_entry: value.fee_per_write_entry,
            fee_per_read_1kb: value.fee_per_read_1kb,
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_historical_1kb: value.fee_per_historical_1kb,
            fee_per_metadata_1kb: value.fee_per_metadata_1kb,
            fee_per_propagate_1kb: value.fee_per_propagate_1kb,
        }
    }
}

impl From<&CxxLedgerEntryRentChange> for LedgerEntryRentChange {
    fn from(value: &CxxLedgerEntryRentChange) -> Self {
        Self {
            is_persistent: value.is_persistent,
            old_size_bytes: value.old_size_bytes,
            new_size_bytes: value.new_size_bytes,
            old_expiration_ledger: value.old_expiration_ledger,
            new_expiration_ledger: value.new_expiration_ledger,
        }
    }
}

impl From<CxxRentFeeConfiguration> for RentFeeConfiguration {
    fn from(value: CxxRentFeeConfiguration) -> Self {
        Self {
            fee_per_write_1kb: value.fee_per_write_1kb,
            persistent_rent_rate_denominator: value.persistent_rent_rate_denominator,
            temporary_rent_rate_denominator: value.temporary_rent_rate_denominator,
        }
    }
}

impl From<CxxWriteFeeConfiguration> for WriteFeeConfiguration {
    fn from(value: CxxWriteFeeConfiguration) -> Self {
        Self {
            bucket_list_target_size_bytes: value.bucket_list_target_size_bytes,
            write_fee_1kb_bucket_list_low: value.write_fee_1kb_bucket_list_low,
            write_fee_1kb_bucket_list_high: value.write_fee_1kb_bucket_list_high,
            bucket_list_write_fee_growth_factor: value.bucket_list_write_fee_growth_factor,
        }
    }
}

#[derive(Debug)]
pub(crate) enum CoreHostError {
    Host(HostError),
    General(&'static str),
}

impl Display for CoreHostError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl From<HostError> for CoreHostError {
    fn from(h: HostError) -> Self {
        CoreHostError::Host(h)
    }
}

impl From<xdr::Error> for CoreHostError {
    fn from(_: xdr::Error) -> Self {
        CoreHostError::Host((ScErrorType::Value, ScErrorCode::InvalidInput).into())
    }
}

impl std::error::Error for CoreHostError {}

fn xdr_from_slice<T: ReadXdr>(v: &[u8]) -> Result<T, HostError> {
    Ok(T::read_xdr(&mut Cursor::new(v))
        .map_err(|_| (ScErrorType::Value, ScErrorCode::InvalidInput))?)
}

fn xdr_to_vec_u8<T: WriteXdr>(t: &T) -> Result<Vec<u8>, HostError> {
    let mut vec: Vec<u8> = Vec::new();
    t.write_xdr(&mut Cursor::new(&mut vec))
        .map_err(|_| (ScErrorType::Value, ScErrorCode::InvalidInput))?;
    Ok(vec)
}

fn xdr_to_rust_buf<T: WriteXdr>(t: &T) -> Result<RustBuf, HostError> {
    let data = xdr_to_vec_u8(t)?;
    Ok(RustBuf { data })
}

fn event_to_diagnostic_event_rust_buf(
    ev: &ContractEvent,
    failed_call: bool,
) -> Result<RustBuf, HostError> {
    let dev = DiagnosticEvent {
        in_successful_contract_call: !failed_call,
        event: ev.clone(),
    };
    xdr_to_rust_buf(&dev)
}

fn xdr_from_cxx_buf<T: ReadXdr>(buf: &CxxBuf) -> Result<T, HostError> {
    xdr_from_slice(buf.data.as_slice())
}

/// Returns a vec of [`XDRFileHash`] structs each representing one .x file
/// that served as input to xdrgen, which created the XDR definitions used in
/// the Rust crates visible here. This allows the C++ side of the bridge
/// to confirm that the same definitions are compiled into the C++ code.
pub fn get_xdr_hashes() -> Vec<XDRFileHash> {
    XDR_FILES_SHA256
        .iter()
        .map(|(file, hash)| XDRFileHash {
            file: (*file).into(),
            hash: (*hash).into(),
        })
        .collect()
}

/// Helper for [`build_storage_footprint_from_xdr`] that inserts a copy of some
/// [`AccessType`] `ty` into a [`storage::Footprint`] for every [`LedgerKey`] in
/// `keys`.
fn populate_access_map(
    access: &mut FootprintMap,
    keys: Vec<LedgerKey>,
    ty: AccessType,
    budget: &Budget,
) -> Result<(), CoreHostError> {
    for lk in keys {
        match lk {
            LedgerKey::Account(_)
            | LedgerKey::Trustline(_)
            | LedgerKey::ContractData(_)
            | LedgerKey::ContractCode(_) => (),
            _ => return Err(CoreHostError::General("unexpected ledger entry type")),
        };
        *access = access.insert(Rc::new(lk), ty.clone(), budget)?;
    }
    Ok(())
}

/// Deserializes an [`xdr::LedgerFootprint`] from the provided [`CxxBuf`], converts
/// its entries to an [`OrdMap`], and returns a [`storage::Footprint`]
/// containing that map.
fn build_storage_footprint_from_xdr(
    budget: &Budget,
    footprint: &xdr::LedgerFootprint,
) -> Result<Footprint, CoreHostError> {
    let xdr::LedgerFootprint {
        read_only,
        read_write,
    } = footprint;
    let mut access = FootprintMap::new();

    populate_access_map(
        &mut access,
        read_only.to_vec(),
        AccessType::ReadOnly,
        budget,
    )?;
    populate_access_map(
        &mut access,
        read_write.to_vec(),
        AccessType::ReadWrite,
        budget,
    )?;
    Ok(storage::Footprint(access))
}

fn ledger_entry_to_ledger_key(le: &LedgerEntry) -> Result<LedgerKey, CoreHostError> {
    match &le.data {
        LedgerEntryData::Account(a) => Ok(LedgerKey::Account(LedgerKeyAccount {
            account_id: a.account_id.clone(),
        })),
        LedgerEntryData::Trustline(tl) => Ok(LedgerKey::Trustline(LedgerKeyTrustLine {
            account_id: tl.account_id.clone(),
            asset: tl.asset.clone(),
        })),
        LedgerEntryData::ContractData(cd) => Ok(LedgerKey::ContractData(LedgerKeyContractData {
            contract: cd.contract.clone(),
            key: cd.key.clone(),
            durability: cd.durability.clone(),
            body_type: match &cd.body {
                ContractDataEntryBody::DataEntry(_data) => ContractEntryBodyType::DataEntry,
                ContractDataEntryBody::ExpirationExtension => {
                    ContractEntryBodyType::ExpirationExtension
                }
            },
        })),
        LedgerEntryData::ContractCode(code) => Ok(LedgerKey::ContractCode(LedgerKeyContractCode {
            hash: code.hash.clone(),
            body_type: match &code.body {
                ContractCodeEntryBody::DataEntry(_data) => ContractEntryBodyType::DataEntry,
                ContractCodeEntryBody::ExpirationExtension => {
                    ContractEntryBodyType::ExpirationExtension
                }
            },
        })),
        _ => Err(CoreHostError::General("unexpected ledger key")),
    }
}

/// Deserializes a sequence of [`xdr::LedgerEntry`] structures from a vector of
/// [`CxxBuf`] buffers, inserts them into an [`OrdMap`] with entries
/// additionally keyed by the provided contract ID, checks that the entries
/// match the provided [`storage::Footprint`], and returns the constructed map.
fn build_storage_map_from_xdr_ledger_entries(
    budget: &Budget,
    footprint: &storage::Footprint,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<StorageMap, CoreHostError> {
    let mut map = StorageMap::new();
    for buf in ledger_entries {
        let le = Rc::new(xdr_from_cxx_buf::<LedgerEntry>(buf)?);
        let key = Rc::new(ledger_entry_to_ledger_key(&le)?);
        if !footprint.0.contains_key::<LedgerKey>(&key, budget)? {
            return Err(CoreHostError::General(
                "ledger entry not found in footprint",
            ));
        }
        map = map.insert(key, Some(le), budget)?;
    }
    for k in footprint.0.keys(budget)? {
        if !map.contains_key::<LedgerKey>(k, budget)? {
            map = map.insert(k.clone(), None, budget)?;
        }
    }
    Ok(map)
}

fn build_auth_entries_from_xdr(
    contract_auth_entries_xdr: &Vec<CxxBuf>,
) -> Result<Vec<SorobanAuthorizationEntry>, CoreHostError> {
    let mut res = vec![];
    for buf in contract_auth_entries_xdr {
        res.push(xdr_from_cxx_buf::<SorobanAuthorizationEntry>(buf)?);
    }
    Ok(res)
}

/// Iterates over the storage map and serializes the read-write ledger entries
/// back to XDR.
fn build_xdr_ledger_entries_from_storage_map(
    footprint: &storage::Footprint,
    storage_map: &StorageMap,
    budget: &Budget,
) -> Result<Vec<RustBuf>, CoreHostError> {
    let mut res = Vec::new();
    for (lk, ole) in storage_map {
        match footprint.0.get::<LedgerKey>(lk, budget)? {
            Some(AccessType::ReadOnly) => (),
            Some(AccessType::ReadWrite) => {
                if let Some(le) = ole {
                    res.push(xdr_to_rust_buf(&**le)?)
                }
            }
            None => return Err(CoreHostError::General("ledger entry not in footprint")),
        }
    }
    Ok(res)
}

fn extract_contract_events(events: &Events) -> Result<Vec<RustBuf>, HostError> {
    events
        .0
        .iter()
        .filter_map(|e| {
            if e.failed_call || e.event.type_ == ContractEventType::Diagnostic {
                return None;
            }

            Some(xdr_to_rust_buf(&e.event))
        })
        .collect()
}

// Note that this also includes Contract events so the ordering between
// Contract and StructuredDebug events can be preserved. This does mean that
// the Contract events will be duplicated in the meta if diagnostics are on - in
// the hashed portion of the meta, and in the non-hashed diagnostic events.
fn extract_diagnostic_events(events: &Events) -> Result<Vec<RustBuf>, HostError> {
    events
        .0
        .iter()
        .map(|e| event_to_diagnostic_event_rust_buf(&e.event, e.failed_call))
        .collect()
}

fn log_debug_events(events: &Events) {
    for e in events.0.iter() {
        match &e.event.type_ {
            ContractEventType::Contract | ContractEventType::System => (),
            ContractEventType::Diagnostic => {
                debug!("Diagnostic event: {:?}", e.event)
            }
        }
    }
}

fn extract_bumps(bumps: &ExpirationLedgerBumps) -> Result<Vec<Bump>, HostError> {
    bumps
        .iter()
        .map(|e| {
            Ok(Bump {
                ledger_key: xdr_to_rust_buf(e.key.as_ref())?,
                min_expiration: e.min_expiration,
            })
        })
        .collect()
}

/// Deserializes an [`xdr::HostFunction`] host function XDR object an
/// [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the specified host function
/// and returns the [`InvokeHostFunctionOutput`] that contains the host function
/// result, events and modified ledger entries. Ledger entries not returned have
/// been deleted.
pub(crate) fn invoke_host_function(
    enable_diagnostics: bool,
    hf_buf: &CxxBuf,
    resources_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_function_or_maybe_panic(
            enable_diagnostics,
            hf_buf,
            resources_buf,
            source_account_buf,
            auth_entries,
            ledger_info,
            ledger_entries,
            base_prng_seed,
        )
    }));
    match res {
        Err(_) => Err(CoreHostError::General("contract host panicked").into()),
        Ok(r) => r,
    }
}

fn invoke_host_function_or_maybe_panic(
    enable_diagnostics: bool,
    hf_buf: &CxxBuf,
    resources_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let _client = Client::start();
    let _span = span!("invoke_host_function");

    let hf = xdr_from_cxx_buf::<HostFunction>(&hf_buf)?;
    let source_account = xdr_from_cxx_buf::<AccountId>(&source_account_buf)?;
    let resources = xdr_from_cxx_buf::<SorobanResources>(&resources_buf)?;

    let budget = Budget::from_configs(
        resources.instructions as u64,
        ledger_info.memory_limit as u64,
        xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.cpu_cost_params)?,
        xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.mem_cost_params)?,
    );
    let footprint = build_storage_footprint_from_xdr(&budget, &resources.footprint)?;
    let map = build_storage_map_from_xdr_ledger_entries(&budget, &footprint, ledger_entries)?;
    let storage = Storage::with_enforcing_footprint_and_map(footprint, map);
    let auth_entries = build_auth_entries_from_xdr(auth_entries)?;
    let host = Host::with_storage_and_budget(storage, budget);
    host.set_source_account(source_account)?;
    host.set_ledger_info(ledger_info.into())?;
    host.set_authorization_entries(auth_entries)?;
    let seed32: [u8; 32] = base_prng_seed
        .data
        .as_slice()
        .try_into()
        .map_err(|_| CoreHostError::General("Wrong base PRNG seed size"))?;
    host.set_base_prng_seed(seed32)?;
    if enable_diagnostics {
        host.set_diagnostic_level(DiagnosticLevel::Debug)?;
    }

    let res = host.invoke_function(hf);
    let (storage, budget, events, bumps) = host
        .try_finish()
        .map_err(|_h| CoreHostError::General("could not finalize host"))?;
    log_debug_events(&events);
    let result_value = match res {
        Ok(rv) => xdr_to_rust_buf(&rv)?,
        Err(err) => {
            debug!(target: TX, "invocation failed: {}", err);
            return Ok(InvokeHostFunctionOutput {
                success: false,
                result_value: RustBuf { data: vec![] },
                contract_events: vec![],
                diagnostic_events: extract_diagnostic_events(&events)?,
                modified_ledger_entries: vec![],
                cpu_insns: budget.get_cpu_insns_consumed()?,
                mem_bytes: budget.get_mem_bytes_consumed()?,
                expiration_bumps: vec![],
            });
        }
    };

    let modified_ledger_entries =
        build_xdr_ledger_entries_from_storage_map(&storage.footprint, &storage.map, &budget)?;
    let contract_events = extract_contract_events(&events)?;
    let diagnostic_events = extract_diagnostic_events(&events)?;
    let expiration_bumps = extract_bumps(&bumps)?;

    Ok(InvokeHostFunctionOutput {
        success: true,
        result_value,
        contract_events,
        diagnostic_events,
        modified_ledger_entries,
        cpu_insns: budget.get_cpu_insns_consumed()?,
        mem_bytes: budget.get_mem_bytes_consumed()?,
        expiration_bumps,
    })
}

pub(crate) fn compute_transaction_resource_fee(
    tx_resources: CxxTransactionResources,
    fee_config: CxxFeeConfiguration,
) -> FeePair {
    let (non_refundable_fee, refundable_fee) =
        host_compute_transaction_resource_fee(&tx_resources.into(), &fee_config.into());
    FeePair {
        non_refundable_fee,
        refundable_fee,
    }
}

pub(crate) fn compute_rent_fee(
    changed_entries: &Vec<CxxLedgerEntryRentChange>,
    fee_config: CxxRentFeeConfiguration,
    current_ledger_seq: u32,
) -> i64 {
    let changed_entries = changed_entries.iter().map(|e| e.into()).collect();
    host_compute_rent_fee(&changed_entries, &fee_config.into(), current_ledger_seq)
}

pub(crate) fn compute_write_fee_per_1kb(
    bucket_list_size: i64,
    fee_config: CxxWriteFeeConfiguration,
) -> i64 {
    host_compute_write_fee_per_1kb(bucket_list_size, &fee_config.into())
}
