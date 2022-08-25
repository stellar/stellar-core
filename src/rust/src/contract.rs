// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBuf, CxxLedgerInfo, InvokeHostFunctionOutput, PreflightCallbacks, PreflightHostFunctionOutput, RustBuf, XDRFileHash
    },
};
use cxx::{CxxVector, UniquePtr};
use log::debug;
use std::{cell::RefCell, fmt::Display, io::Cursor, panic, pin::Pin, rc::Rc};

use soroban_env_host::{
    budget::Budget,
    events::{Events, HostEvent},
    storage::{self, AccessType, SnapshotSource, Storage},
    xdr,
    xdr::{
        AccountId, HostFunction, LedgerEntry, LedgerEntryData, LedgerFootprint, LedgerKey,
        LedgerKeyAccount, LedgerKeyContractData, LedgerKeyTrustLine, ReadXdr,
        ScHostContextErrorCode, ScUnknownErrorCode, ScVec, WriteXdr, XDR_FILES_SHA256,
    },
    Host, HostError, LedgerInfo, MeteredOrdMap,
};
use std::error::Error;

impl From<CxxLedgerInfo> for LedgerInfo {
    fn from(c: CxxLedgerInfo) -> Self {
        Self {
            protocol_version: c.protocol_version,
            sequence_number: c.sequence_number,
            timestamp: c.timestamp,
            network_passphrase: c.network_passphrase,
            base_reserve: c.base_reserve,
        }
    }
}

#[derive(Debug)]
enum CoreHostError {
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
    access: &mut MeteredOrdMap<LedgerKey, AccessType>,
    keys: Vec<LedgerKey>,
    ty: AccessType,
) -> Result<(), CoreHostError> {
    for lk in keys {
        match lk {
            LedgerKey::Account(_) | LedgerKey::Trustline(_) | LedgerKey::ContractData(_) => (),
            _ => return Err(CoreHostError::General("unexpected ledger entry type")),
        };
        access.insert(lk, ty.clone())?;
    }
    Ok(())
}

/// Deserializes an [`xdr::LedgerFootprint`] from the provided [`CxxBuf`], converts
/// its entries to an [`OrdMap`], and returns a [`storage::Footprint`]
/// containing that map.
fn build_storage_footprint_from_xdr(
    budget: Budget,
    footprint: &CxxBuf,
) -> Result<storage::Footprint, CoreHostError> {
    let xdr::LedgerFootprint {
        read_only,
        read_write,
    } = xdr::LedgerFootprint::read_xdr(&mut Cursor::new(footprint.data.as_slice()))?;
    let mut access = MeteredOrdMap::new(budget)?;

    populate_access_map(&mut access, read_only.to_vec(), AccessType::ReadOnly)?;
    populate_access_map(&mut access, read_write.to_vec(), AccessType::ReadWrite)?;
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
        _ => Err(CoreHostError::General("unexpected ledger key")),
    }
}

/// Deserializes a sequence of [`xdr::LedgerEntry`] structures from a vector of
/// [`CxxBuf`] buffers, inserts them into an [`OrdMap`] with entries
/// additionally keyed by the provided contract ID, checks that the entries
/// match the provided [`storage::Footprint`], and returns the constructed map.
fn build_storage_map_from_xdr_ledger_entries(
    budget: Budget,
    footprint: &storage::Footprint,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<MeteredOrdMap<LedgerKey, Option<LedgerEntry>>, CoreHostError> {
    let mut map = MeteredOrdMap::new(budget)?;
    for buf in ledger_entries {
        let le = xdr_from_cxx_buf::<LedgerEntry>(buf)?;
        let key = ledger_entry_to_ledger_key(&le)?;
        if !footprint.0.contains_key(&key)? {
            return Err(CoreHostError::General(
                "ledger entry not found in footprint",
            ));
        }
        map.insert(key, Some(le))?;
    }
    for k in footprint.0.keys()? {
        if !map.contains_key(k)? {
            map.insert(k.clone(), None)?;
        }
    }
    Ok(map)
}

/// Iterates over the storage map and serializes the read-write ledger entries
/// back to XDR.
fn build_xdr_ledger_entries_from_storage_map(
    footprint: &storage::Footprint,
    storage_map: &MeteredOrdMap<LedgerKey, Option<LedgerEntry>>,
) -> Result<Vec<RustBuf>, CoreHostError> {
    let mut res = Vec::new();
    for (lk, ole) in storage_map {
        match footprint.0.get(lk)? {
            Some(AccessType::ReadOnly) => (),
            Some(AccessType::ReadWrite) => {
                if let Some(le) = ole {
                    res.push(xdr_to_rust_buf(le)?)
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
        .filter_map(|e| match e {
            HostEvent::Contract(ce) => Some(xdr_to_rust_buf(ce)),
            HostEvent::Debug(_) => None,
        })
        .collect()
}

fn log_debug_events(events: &Events) {
    for e in events.0.iter() {
        match e {
            HostEvent::Contract(_) => (),
            HostEvent::Debug(de) => debug!("contract HostEvent::Debug: {}", de),
        }
    }
}

/// Deserializes an [`xdr::HostFunction`] host function identifier, an [`xdr::ScVec`] XDR object of
/// arguments, an [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the host function with the specified
/// arguments, discards the [`xdr::ScVal`] return value, and returns the [`ReadWrite`] ledger
/// entries in serialized form. Ledger entries not returned have been deleted.

pub(crate) fn invoke_host_function(
    hf_buf: &CxxBuf,
    args_buf: &CxxBuf,
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_function_or_maybe_panic(
            hf_buf,
            args_buf,
            footprint_buf,
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

fn invoke_host_function_or_maybe_panic(
    hf_buf: &CxxBuf,
    args_buf: &CxxBuf,
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let budget = Budget::default();
    let hf = xdr_from_cxx_buf::<HostFunction>(&hf_buf)?;
    let args = xdr_from_cxx_buf::<ScVec>(&args_buf)?;
    let source_account = xdr_from_cxx_buf::<AccountId>(&source_account_buf)?;

    let footprint = build_storage_footprint_from_xdr(budget.clone(), footprint_buf)?;
    let map =
        build_storage_map_from_xdr_ledger_entries(budget.clone(), &footprint, ledger_entries)?;

    let storage = Storage::with_enforcing_footprint_and_map(footprint, map);
    let host = Host::with_storage_and_budget(storage, budget);
    host.set_source_account(source_account);
    host.set_ledger_info(ledger_info.into());

    debug!(target: TX, "invoking host function '{}'", HostFunction::name(&hf));
    let res = host.invoke_function(hf, args);
    let (storage, _budget, events) = host
        .try_finish()
        .map_err(|_h| CoreHostError::General("could not finalize host"))?;
    log_debug_events(&events);
    match res {
        Ok(_) => (),
        Err(err) => {
            debug!(target: TX, "invocation failed: {}", err);
            return Err(err.into());
        }
    }
    let modified_ledger_entries =
        build_xdr_ledger_entries_from_storage_map(&storage.footprint, &storage.map)?;
    let contract_events = extract_contract_events(&events)?;
    Ok(InvokeHostFunctionOutput {
        contract_events,
        modified_ledger_entries,
    })
}

// Pfc here exists just to translate a mess of irrelevant ownership and access
// quirks between UniquePtr<PreflightCallbacks>, Rc<dyn SnapshotSource> and
// Pin<&mut PreflightCallbacks>. They're all pointers to the same thing (or
// perhaps indirected through an Rc or RefCell) but unfortunately, as you know,
// type systems.
struct Pfc(RefCell<UniquePtr<PreflightCallbacks>>);

impl Pfc {
    fn with_cb<T, F>(&self, f: F) -> Result<T, HostError>
    where
        F: FnOnce(Pin<&mut PreflightCallbacks>) -> Result<T, ::cxx::Exception>,
    {
        if let Some(cb) = self
            .0
            .try_borrow_mut()
            .map_err(|_| ScHostContextErrorCode::UnknownError)?
            .as_mut()
        {
            f(cb).map_err(|_| ScHostContextErrorCode::UnknownError.into())
        } else {
            Err(ScHostContextErrorCode::UnknownError.into())
        }
    }
}

impl SnapshotSource for Pfc {
    fn get(&self, key: &LedgerKey) -> Result<LedgerEntry, HostError> {
        let kv = xdr_to_vec_u8(key)?;
        let lev = self.with_cb(|cb| cb.get_ledger_entry(&kv))?;
        xdr_from_cxx_buf(&lev)
    }

    fn has(&self, key: &LedgerKey) -> Result<bool, HostError> {
        let kv = xdr_to_vec_u8(key)?;
        self.with_cb(|cb| cb.has_ledger_entry(&kv))
    }
}

fn storage_footprint_to_ledger_footprint(
    foot: &storage::Footprint,
) -> Result<LedgerFootprint, xdr::Error> {
    let mut read_only = Vec::new();
    let mut read_write = Vec::new();
    for (k, v) in foot.0.iter() {
        match v {
            AccessType::ReadOnly => read_only.push(k.clone()),
            AccessType::ReadWrite => read_write.push(k.clone()),
        }
    }
    Ok(LedgerFootprint {
        read_only: read_only.try_into()?,
        read_write: read_write.try_into()?,
    })
}

pub(crate) fn preflight_host_function(
    hf_buf: &CxxVector<u8>,
    args_buf: &CxxVector<u8>,
    source_account_buf: &CxxVector<u8>,
    ledger_info: CxxLedgerInfo,
    cb: UniquePtr<PreflightCallbacks>,
) -> Result<PreflightHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        preflight_host_function_or_maybe_panic(hf_buf, args_buf, source_account_buf, ledger_info, cb)
    }));
    match res {
        Err(_) => Err(CoreHostError::General("contract host panicked").into()),
        Ok(r) => r,
    }
}

fn preflight_host_function_or_maybe_panic(
    hf_buf: &CxxVector<u8>,
    args_buf: &CxxVector<u8>,
    source_account_buf: &CxxVector<u8>,
    ledger_info: CxxLedgerInfo,
    cb: UniquePtr<PreflightCallbacks>,
) -> Result<PreflightHostFunctionOutput, Box<dyn Error>> {
    let hf = xdr_from_slice::<HostFunction>(hf_buf.as_slice())?;
    let args = xdr_from_slice::<ScVec>(args_buf.as_slice())?;
    let source_account = xdr_from_slice::<AccountId>(source_account_buf.as_slice())?;
    let pfc = Rc::new(Pfc(RefCell::new(cb)));
    let src: Rc<dyn SnapshotSource> = pfc.clone() as Rc<dyn SnapshotSource>;
    let storage = Storage::with_recording_footprint(src);
    let budget = Budget::default();
    let host = Host::with_storage_and_budget(storage, budget);
    host.set_source_account(source_account);
    host.set_ledger_info(ledger_info.into());

    debug!(
        target: TX,
        "preflight execution of host function '{}'",
        HostFunction::name(&hf)
    );
    let res = host.invoke_function(hf, args);

    // Recover, convert and return the storage footprint and other values to C++.
    let (storage, budget, events) = host
        .try_finish()
        .map_err(|_| CoreHostError::General("could not finalize host"))?;
    log_debug_events(&events);
    let val = match res {
        Ok(val) => val,
        Err(err) => {
            debug!(target: TX, "preflight failed: {}", err);
            return Err(err.into());
        }
    };

    let storage_footprint =
        xdr_to_rust_buf(&storage_footprint_to_ledger_footprint(&storage.footprint)?)?;
    let contract_events = extract_contract_events(&events)?;
    let result_value = xdr_to_rust_buf(&val)?;

    Ok(PreflightHostFunctionOutput {
        result_value,
        contract_events,
        storage_footprint,
        cpu_insns: budget.get_cpu_insns_count(),
        mem_bytes: budget.get_mem_bytes_count(),
    })
}

// Accessors for test wasms, compiled into soroban-test-wasms crate.
pub(crate) fn get_test_wasm_add_i32() -> Result<RustBuf, Box<dyn Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::ADD_I32.iter().cloned().collect(),
    })
}
pub(crate) fn get_test_wasm_contract_data() -> Result<RustBuf, Box<dyn Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONTRACT_DATA.iter().cloned().collect(),
    })
}
