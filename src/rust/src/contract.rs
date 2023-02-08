// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBuf, CxxLedgerInfo, InvokeHostFunctionOutput, PreflightCallbacks,
        PreflightHostFunctionOutput, RustBuf, XDRFileHash,
    },
};
use cxx::{CxxVector, UniquePtr};
use log::debug;
use std::{cell::RefCell, fmt::Display, io::Cursor, panic, pin::Pin, rc::Rc};

use soroban_env_host::{
    budget::Budget,
    events::{DebugError, DebugEvent, Events, HostEvent},
    storage::{self, AccessType, Footprint, FootprintMap, SnapshotSource, Storage, StorageMap},
    xdr,
    xdr::{
        AccountId, ContractAuth, HostFunction, LedgerEntry, LedgerEntryData, LedgerFootprint,
        LedgerKey, LedgerKeyAccount, LedgerKeyContractCode, LedgerKeyContractData,
        LedgerKeyTrustLine, ReadXdr, ScHostContextErrorCode, ScUnknownErrorCode, WriteXdr,
        XDR_FILES_SHA256,
    },
    Host, HostError, LedgerInfo,
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
    let mut access = FootprintMap::new(budget)?;

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
    let mut map = StorageMap::new(budget)?;
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

/// Deserializes an [`xdr::HostFunction`] host function XDR object an
/// [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the specified host function
/// and returns the [`InvokeHostFunctionOutput`] that contains the host function
/// result, events and modified ledger entries. Ledger entries not returned have
/// been deleted.
pub(crate) fn invoke_host_function(
    hf_buf: &CxxBuf,
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    contract_auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_function_or_maybe_panic(
            hf_buf,
            footprint_buf,
            source_account_buf,
            contract_auth_entries,
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
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    contract_auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let budget = Budget::default();
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
            return Err(err.into());
        }
    };
    let modified_ledger_entries =
        build_xdr_ledger_entries_from_storage_map(&storage.footprint, &storage.map, &budget)?;
    let contract_events = extract_contract_events(&events)?;
    Ok(InvokeHostFunctionOutput {
        result_value,
        contract_events,
        modified_ledger_entries,
        cpu_insns: budget.get_cpu_insns_count(),
        mem_bytes: budget.get_mem_bytes_count(),
    })
}

// Pfc here exists just to translate a mess of irrelevant ownership and access
// quirks between UniquePtr<PreflightCallbacks>, Rc<dyn SnapshotSource> and
// Pin<&mut PreflightCallbacks>. They're all pointers to the same thing (or
// perhaps indirected through an Rc or RefCell) but unfortunately, as you know,
// type systems.
//
// Pfc also carries a reference to a Host to allow us to record as debug events
// any strings returned in exceptions thrown while running the callbacks. This
// is awkward but it's the best we can do for mapping from the core error regime
// to that of the Host.
struct Pfc(
    RefCell<Option<Host>>,
    RefCell<UniquePtr<PreflightCallbacks>>,
);

impl Pfc {
    fn with_cb<T, F>(&self, f: F) -> Result<T, HostError>
    where
        F: FnOnce(Pin<&mut PreflightCallbacks>) -> Result<T, ::cxx::Exception>,
    {
        let code = ScHostContextErrorCode::UnknownError;

        if let Some(cb) = self.1.try_borrow_mut().map_err(|_| code)?.as_mut() {
            f(cb).map_err(|exn| {
                // Error propagation is relatively awkward here. We need to
                // generate a `HostError`, which has no string, and we have a
                // `cxx::Exception`, which only has a string. We therefore have
                // a `Host` reference plumbed through here so that we can call
                // `err` on it passing a `DebugError` carrying a copy of the
                // string from the `cxx::Exception`. This will in turn record
                // the string in the `Host` debug-event buffer and then generate
                // a `HostError` carrying a snapshot of that buffer, which will
                // hopefully make it back to users.
                //
                // NB: the `Host` reference in this function is only here to
                // serve this purpose; it's not otherwise needed.
                if let Ok(Some(host)) = self.0.try_borrow().map(|r| (*r).clone()) {
                    let err = DebugError {
                        event: DebugEvent::new().msg(exn.what().to_string()),
                        status: code.into(),
                    };
                    host.err(err)
                } else {
                    code.into()
                }
            })
        } else {
            Err(code.into())
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
    budget: &Budget,
) -> Result<LedgerFootprint, Box<dyn Error>> {
    let mut read_only = Vec::new();
    let mut read_write = Vec::new();
    for (k, v) in foot.0.iter(budget)? {
        match v {
            AccessType::ReadOnly => read_only.push(k.clone()),
            AccessType::ReadWrite => read_write.push(k.clone()),
        }
    }
    let read_only_not_boxed: Vec<LedgerKey> = read_only.into_iter().map(|x| (*x).clone()).collect();
    let read_write_not_boxed: Vec<LedgerKey> =
        read_write.into_iter().map(|x| (*x).clone()).collect();
    Ok(LedgerFootprint {
        read_only: read_only_not_boxed.try_into()?,
        read_write: read_write_not_boxed.try_into()?,
    })
}

pub(crate) fn preflight_host_function(
    hf_buf: &CxxVector<u8>,
    source_account_buf: &CxxVector<u8>,
    ledger_info: CxxLedgerInfo,
    cb: UniquePtr<PreflightCallbacks>,
) -> Result<PreflightHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        preflight_host_function_or_maybe_panic(hf_buf, source_account_buf, ledger_info, cb)
    }));
    match res {
        Err(_) => Err(CoreHostError::General("contract host panicked").into()),
        Ok(r) => r,
    }
}

fn preflight_host_function_or_maybe_panic(
    hf_buf: &CxxVector<u8>,
    source_account_buf: &CxxVector<u8>,
    ledger_info: CxxLedgerInfo,
    cb: UniquePtr<PreflightCallbacks>,
) -> Result<PreflightHostFunctionOutput, Box<dyn Error>> {
    let hf = xdr_from_slice::<HostFunction>(hf_buf.as_slice())?;
    let source_account = xdr_from_slice::<AccountId>(source_account_buf.as_slice())?;
    let pfc = Rc::new(Pfc(RefCell::new(None), RefCell::new(cb)));
    let src: Rc<dyn SnapshotSource> = pfc.clone() as Rc<dyn SnapshotSource>;
    let storage = Storage::with_recording_footprint(src);
    let budget = Budget::default();
    let host = Host::with_storage_and_budget(storage, budget);

    host.set_source_account(source_account);
    host.set_ledger_info(ledger_info.into());
    host.switch_to_recording_auth();

    debug!(
        target: TX,
        "preflight execution of host function '{}'",
        HostFunction::name(&hf)
    );

    // NB: this line creates cyclical ownership between Pfc and Host. We need to
    // do this so that Pfc can access Host in `with_cb` above, but we must break
    // that cycle below, otherwise both will leak.
    *pfc.0.try_borrow_mut()? = Some(host.clone());

    // Run the preflight.
    let res = host.invoke_function(hf);

    // Break cyclical ownership between Pfc and Host.
    *pfc.0.try_borrow_mut()? = None;

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

    let storage_footprint = xdr_to_rust_buf(&storage_footprint_to_ledger_footprint(
        &storage.footprint,
        &budget,
    )?)?;
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

pub(crate) fn get_test_wasm_complex() -> Result<RustBuf, Box<dyn Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::COMPLEX.iter().cloned().collect(),
    })
}
