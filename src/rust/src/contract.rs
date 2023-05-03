// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBuf, CxxFeeConfiguration, CxxLedgerInfo, CxxTransactionResources, FeePair,
        InvokeHostFunctionOutput, RustBuf, XDRFileHash,
    },
};
use log::debug;
use soroban_env_host_curr::xdr::ContractCostParams;
use std::{fmt::Display, io::Cursor, panic, rc::Rc};

// This module (contract) is bound to _two separate locations_ in the module
// tree: crate::lo::contract and crate::hi::contract, each of which has a (lo or
// hi) version-specific definition of stellar_env_host. We therefore
// import it from our _parent_ module rather than from the crate root.
use super::soroban_env_host::{
    budget::Budget,
    events::{Event, Events},
    fees::{
        compute_transaction_resource_fee as host_compute_transaction_resource_fee,
        FeeConfiguration, TransactionResources,
    },
    storage::{self, AccessType, Footprint, FootprintMap, Storage, StorageMap},
    xdr::{
        self, AccountId, ContractEvent, DiagnosticEvent, HostFunction, LedgerEntry,
        LedgerEntryData, LedgerKey, LedgerKeyAccount, LedgerKeyContractCode, LedgerKeyContractData,
        LedgerKeyTrustLine, ReadXdr, ScUnknownErrorCode, SorobanResources, WriteXdr,
        XDR_FILES_SHA256,
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
        CoreHostError::Host(ScUnknownErrorCode::Xdr.into())
    }
}

impl std::error::Error for CoreHostError {}

fn xdr_from_slice<T: ReadXdr>(v: &[u8]) -> Result<T, HostError> {
    Ok(T::read_xdr(&mut Cursor::new(v)).map_err(|_| ScUnknownErrorCode::Xdr)?)
}

fn xdr_to_vec_u8<T: WriteXdr>(t: &T) -> Result<Vec<u8>, HostError> {
    let mut vec: Vec<u8> = Vec::new();
    t.write_xdr(&mut Cursor::new(&mut vec))
        .map_err(|_| ScUnknownErrorCode::Xdr)?;
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
    let mut access = FootprintMap::new()?;

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
            contract_id: cd.contract_id.clone(),
            key: cd.key.clone(),
        })),
        LedgerEntryData::ContractCode(code) => Ok(LedgerKey::ContractCode(LedgerKeyContractCode {
            hash: code.hash.clone(),
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
    let mut map = StorageMap::new()?;
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
            if e.failed_call {
                return None;
            }

            match &e.event {
                Event::Contract(ce) => Some(xdr_to_rust_buf(ce)),
                Event::Debug(_) => None,
                Event::StructuredDebug(_) => None,
            }
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
        .filter_map(|e| match &e.event {
            Event::Contract(ce) => Some(event_to_diagnostic_event_rust_buf(&ce, e.failed_call)),
            Event::Debug(_) => None,
            Event::StructuredDebug(ce) => {
                Some(event_to_diagnostic_event_rust_buf(&ce, e.failed_call))
            }
        })
        .collect()
}

fn log_debug_events(events: &Events) {
    for e in events.0.iter() {
        match &e.event {
            Event::Contract(_) => (),
            Event::Debug(de) => debug!("contract HostEvent::Debug: {}", de),
            Event::StructuredDebug(sd) => debug!("contract HostEvent::StructuredDebug: {:?}", sd),
        }
    }
}

/// Deserializes an [`xdr::HostFunction`] host function XDR object an
/// [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the specified host function
/// and returns the [`InvokeHostFunctionOutput`] that contains the host function
/// result, events and modified ledger entries. Ledger entries not returned have
/// been deleted.
pub(crate) fn invoke_host_functions(
    enable_diagnostics: bool,
    hf_bufs: &Vec<CxxBuf>,
    resources_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_functions_or_maybe_panic(
            enable_diagnostics,
            hf_bufs,
            resources_buf,
            source_account_buf,
            ledger_info,
            ledger_entries,
        )
    }));
    match res {
        Err(_) => Err(CoreHostError::General("contract host panicked").into()),
        Ok(r) => r,
    }
}

fn invoke_host_functions_or_maybe_panic(
    enable_diagnostics: bool,
    hf_bufs: &Vec<CxxBuf>,
    resources_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let hfs = hf_bufs
        .iter()
        .map(|hf_buf| xdr_from_cxx_buf::<HostFunction>(&hf_buf))
        .collect::<Result<Vec<HostFunction>, HostError>>()?;
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
    let host = Host::with_storage_and_budget(storage, budget);
    host.set_source_account(source_account);
    host.set_ledger_info(ledger_info.into());
    if enable_diagnostics {
        host.set_diagnostic_level(DiagnosticLevel::Debug);
    }

    let res: Result<Vec<xdr::ScVal>, HostError> = host.invoke_functions(hfs);
    let (storage, budget, events) = host
        .try_finish()
        .map_err(|_h| CoreHostError::General("could not finalize host"))?;
    log_debug_events(&events);
    let result_values = match res {
        Ok(rv) => rv
            .iter()
            .map(|v| xdr_to_rust_buf(v))
            .collect::<Result<Vec<RustBuf>, HostError>>()?,
        Err(err) => {
            debug!(target: TX, "invocation failed: {}", err);
            return Ok(InvokeHostFunctionOutput {
                success: false,
                result_values: vec![],
                contract_events: vec![],
                diagnostic_events: extract_diagnostic_events(&events)?,
                modified_ledger_entries: vec![],
                cpu_insns: budget.get_cpu_insns_count(),
                mem_bytes: budget.get_mem_bytes_count(),
            });
        }
    };

    let modified_ledger_entries =
        build_xdr_ledger_entries_from_storage_map(&storage.footprint, &storage.map, &budget)?;
    let contract_events = extract_contract_events(&events)?;
    let diagnostic_events = extract_diagnostic_events(&events)?;
    Ok(InvokeHostFunctionOutput {
        success: true,
        result_values,
        contract_events,
        diagnostic_events,
        modified_ledger_entries,
        cpu_insns: budget.get_cpu_insns_count(),
        mem_bytes: budget.get_mem_bytes_count(),
    })
}

pub(crate) fn compute_transaction_resource_fee(
    tx_resources: CxxTransactionResources,
    fee_config: CxxFeeConfiguration,
) -> FeePair {
    let (fee, refundable_fee) =
        host_compute_transaction_resource_fee(&tx_resources.into(), &fee_config.into());
    FeePair {
        fee,
        refundable_fee,
    }
}
