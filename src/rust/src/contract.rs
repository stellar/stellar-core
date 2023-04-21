// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBudgetConfig, CxxBuf, CxxLedgerInfo, InvokeHostFunctionOutput, RustBuf, XDRFileHash,
    },
};
use log::debug;
use std::{fmt::Display, io::Cursor, panic, rc::Rc};

// This module (contract) is bound to _two separate locations_ in the module
// tree: crate::lo::contract and crate::hi::contract, each of which has a (lo or
// hi) version-specific definition of stellar_env_host. We therefore
// import it from our _parent_ module rather than from the crate root.
use super::soroban_env_host::{
    budget::Budget,
    events::{Event, Events},
    storage::{self, AccessType, Footprint, FootprintMap, Storage, StorageMap},
    xdr::{self, ContractEvent, DiagnosticEvent},
    xdr::{
        AccountId, ContractAuth, ContractCostParams, HostFunction, LedgerEntry, LedgerEntryData,
        LedgerKey, LedgerKeyAccount, LedgerKeyContractCode, LedgerKeyContractData,
        LedgerKeyTrustLine, ReadXdr, ScUnknownErrorCode, WriteXdr, XDR_FILES_SHA256,
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
    footprint: &CxxBuf,
) -> Result<Footprint, CoreHostError> {
    let xdr::LedgerFootprint {
        read_only,
        read_write,
    } = xdr::LedgerFootprint::read_xdr(&mut Cursor::new(footprint.data.as_slice()))?;
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

fn build_contract_auth_entries_from_xdr(
    contract_auth_entries_xdr: &Vec<CxxBuf>,
) -> Result<Vec<ContractAuth>, CoreHostError> {
    let mut res = vec![];
    for buf in contract_auth_entries_xdr {
        res.push(xdr_from_cxx_buf::<ContractAuth>(buf)?);
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
pub(crate) fn invoke_host_function(
    enable_diagnostics: bool,
    hf_buf: &CxxBuf,
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    contract_auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    budget_config: &CxxBudgetConfig,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_function_or_maybe_panic(
            enable_diagnostics,
            hf_buf,
            footprint_buf,
            source_account_buf,
            contract_auth_entries,
            ledger_info,
            ledger_entries,
            budget_config,
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
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    contract_auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    budget_config: &CxxBudgetConfig,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let cpu_cost_params = xdr_from_cxx_buf::<ContractCostParams>(&budget_config.cpu_cost_params)?;
    let mem_cost_params = xdr_from_cxx_buf::<ContractCostParams>(&budget_config.mem_cost_params)?;
    let budget = Budget::from_network_configs(
        budget_config.cpu_limit,
        budget_config.mem_limit,
        cpu_cost_params,
        mem_cost_params,
    );

    let hf = xdr_from_cxx_buf::<HostFunction>(&hf_buf)?;
    let source_account = xdr_from_cxx_buf::<AccountId>(&source_account_buf)?;

    let footprint = build_storage_footprint_from_xdr(&budget, footprint_buf)?;
    let map = build_storage_map_from_xdr_ledger_entries(&budget, &footprint, ledger_entries)?;
    let storage = Storage::with_enforcing_footprint_and_map(footprint, map);
    let auth_entries = build_contract_auth_entries_from_xdr(contract_auth_entries)?;
    let host = Host::with_storage_and_budget(storage, budget);
    host.set_source_account(source_account);
    host.set_ledger_info(ledger_info.into());
    host.set_authorization_entries(auth_entries)?;
    if enable_diagnostics {
        host.set_diagnostic_level(DiagnosticLevel::Debug);
    }

    debug!(
        target: TX,
        "invoking host function '{}'",
        HostFunction::name(&hf)
    );
    let res = host.invoke_function(hf);
    let (storage, budget, events) = host
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
                cpu_insns: 0,
                mem_bytes: 0,
            });
        }
    };

    let modified_ledger_entries =
        build_xdr_ledger_entries_from_storage_map(&storage.footprint, &storage.map, &budget)?;
    let contract_events = extract_contract_events(&events)?;
    let diagnostic_events = extract_diagnostic_events(&events)?;
    Ok(InvokeHostFunctionOutput {
        success: true,
        result_value,
        contract_events,
        diagnostic_events,
        modified_ledger_entries,
        cpu_insns: budget.get_cpu_insns_count(),
        mem_bytes: budget.get_mem_bytes_count(),
    })
}
