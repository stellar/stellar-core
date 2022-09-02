// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{Bytes, PreflightCallbacks, XDRBuf, XDRFileHash},
};
use cxx::{CxxVector, UniquePtr};
use log::info;
use std::{cell::RefCell, fmt::Display, io::Cursor, pin::Pin, rc::Rc};

use soroban_env_host::{
    budget::Budget,
    storage::{self, AccessType, SnapshotSource, Storage},
    xdr,
    xdr::{
        HostFunction, LedgerEntry, LedgerEntryData, LedgerFootprint, LedgerKey, LedgerKeyAccount,
        LedgerKeyContractData, LedgerKeyTrustLine, ReadXdr, ScHostContextErrorCode,
        ScUnknownErrorCode, ScVec, WriteXdr, XDR_FILES_SHA256,
    },
    Host, HostError, MeteredOrdMap,
};
use std::error::Error;

#[derive(Debug)]
enum ContractError {
    Host(HostError),
    General(&'static str),
}

impl Display for ContractError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl From<HostError> for ContractError {
    fn from(h: HostError) -> Self {
        ContractError::Host(h)
    }
}

impl From<xdr::Error> for ContractError {
    fn from(_: xdr::Error) -> Self {
        ContractError::Host(ScUnknownErrorCode::Xdr.into())
    }
}

impl std::error::Error for ContractError {}

fn xdr_from_slice<T: ReadXdr>(v: &[u8]) -> Result<T, HostError> {
    Ok(T::read_xdr(&mut Cursor::new(v)).map_err(|_| ScUnknownErrorCode::Xdr)?)
}

fn xdr_to_vec_u8<T: WriteXdr>(t: &T) -> Result<Vec<u8>, HostError> {
    let mut vec: Vec<u8> = Vec::new();
    t.write_xdr(&mut Cursor::new(&mut vec))
        .map_err(|_| ScUnknownErrorCode::Xdr)?;
    Ok(vec)
}

fn xdr_to_bytes<T: WriteXdr>(t: &T) -> Result<Bytes, HostError> {
    let vec = xdr_to_vec_u8(t)?;
    Ok(Bytes { vec })
}

fn xdr_from_xdrbuf<T: ReadXdr>(buf: &XDRBuf) -> Result<T, HostError> {
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
) -> Result<(), ContractError> {
    for lk in keys {
        match lk {
            LedgerKey::Account(_) | LedgerKey::Trustline(_) | LedgerKey::ContractData(_) => (),
            _ => return Err(ContractError::General("unexpected ledger entry type")),
        };
        access.insert(lk, ty.clone())?;
    }
    Ok(())
}

/// Deserializes an [`xdr::LedgerFootprint`] from the provided [`XDRBuf`], converts
/// its entries to an [`OrdMap`], and returns a [`storage::Footprint`]
/// containing that map.
fn build_storage_footprint_from_xdr(
    budget: Budget,
    footprint: &XDRBuf,
) -> Result<storage::Footprint, ContractError> {
    let xdr::LedgerFootprint {
        read_only,
        read_write,
    } = xdr::LedgerFootprint::read_xdr(&mut Cursor::new(footprint.data.as_slice()))?;
    let mut access = MeteredOrdMap::new(budget)?;

    populate_access_map(&mut access, read_only.to_vec(), AccessType::ReadOnly)?;
    populate_access_map(&mut access, read_write.to_vec(), AccessType::ReadWrite)?;
    Ok(storage::Footprint(access))
}

fn ledger_entry_to_ledger_key(le: &LedgerEntry) -> Result<LedgerKey, ContractError> {
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
        _ => Err(ContractError::General("unexpected ledger key")),
    }
}

/// Deserializes a sequence of [`xdr::LedgerEntry`] structures from a vector of
/// [`XDRBuf`] buffers, inserts them into an [`OrdMap`] with entries
/// additionally keyed by the provided contract ID, checks that the entries
/// match the provided [`storage::Footprint`], and returns the constructed map.
fn build_storage_map_from_xdr_ledger_entries(
    budget: Budget,
    footprint: &storage::Footprint,
    ledger_entries: &Vec<XDRBuf>,
) -> Result<MeteredOrdMap<LedgerKey, Option<LedgerEntry>>, ContractError> {
    let mut map = MeteredOrdMap::new(budget)?;
    for buf in ledger_entries {
        let le = xdr_from_xdrbuf::<LedgerEntry>(buf)?;
        let key = ledger_entry_to_ledger_key(&le)?;
        if !footprint.0.contains_key(&key)? {
            return Err(ContractError::General(
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
) -> Result<Vec<Bytes>, ContractError> {
    let mut res = Vec::new();
    for (lk, ole) in storage_map {
        match footprint.0.get(lk)? {
            Some(AccessType::ReadOnly) => (),
            Some(AccessType::ReadWrite) => {
                if let Some(le) = ole {
                    res.push(xdr_to_bytes(le)?)
                }
            }
            None => return Err(ContractError::General("ledger entry not in footprint")),
        }
    }
    Ok(res)
}

/// Deserializes an [`xdr::HostFunction`] host function identifier, an [`xdr::ScVec`] XDR object of
/// arguments, an [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the host function with the specified
/// arguments, discards the [`xdr::ScVal`] return value, and returns the [`ReadWrite`] ledger
/// entries in serialized form. Ledger entries not returned have been deleted.
pub(crate) fn invoke_host_function(
    hf_buf: &XDRBuf,
    args_buf: &XDRBuf,
    footprint_buf: &XDRBuf,
    ledger_entries: &Vec<XDRBuf>,
) -> Result<Vec<Bytes>, Box<dyn Error>> {
    let budget = Budget::default();
    let hf = xdr_from_xdrbuf::<HostFunction>(&hf_buf)?;
    let args = xdr_from_xdrbuf::<ScVec>(&args_buf)?;

    let footprint = build_storage_footprint_from_xdr(budget.clone(), footprint_buf)?;
    let map =
        build_storage_map_from_xdr_ledger_entries(budget.clone(), &footprint, ledger_entries)?;

    let storage = Storage::with_enforcing_footprint_and_map(footprint, map);
    let host = Host::with_storage_and_budget(storage, budget);

    match hf {
        HostFunction::Call => {
            info!(target: TX, "Invoking host function 'Call'");
            host.invoke_function(hf, args)?;
        }
        HostFunction::CreateContract => {
            info!(target: TX, "Invoking host function 'CreateContract'");
            host.invoke_function(hf, args)?;
        }
    };

    let (storage, _budget, _events) = host
        .try_finish()
        .map_err(|_h| ContractError::General("could not get storage from host"))?;
    Ok(build_xdr_ledger_entries_from_storage_map(
        &storage.footprint,
        &storage.map,
    )?)
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
        xdr_from_xdrbuf(&lev)
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
    cb: UniquePtr<PreflightCallbacks>,
) -> Result<(), Box<dyn Error>> {
    let hf = xdr_from_slice::<HostFunction>(hf_buf.as_slice())?;
    let args = xdr_from_slice::<ScVec>(args_buf.as_slice())?;
    let pfc = Rc::new(Pfc(RefCell::new(cb)));
    let src: Rc<dyn SnapshotSource> = pfc.clone() as Rc<dyn SnapshotSource>;
    let storage = Storage::with_recording_footprint(src);
    let budget = Budget::default();
    let host = Host::with_storage_and_budget(storage, budget);
    let val = host.invoke_function(hf, args)?;

    // Recover, convert and return the storage footprint and other values to C++.
    let (storage, budget, _events) = host
        .try_finish()
        .map_err(|_| ContractError::General("could not get storage from host"))?;
    let (cpu_insns, mem_bytes) = (budget.get_cpu_insns_count(), budget.get_mem_bytes_count());
    let val_vec = xdr_to_vec_u8(&val)?;
    let foot_vec = xdr_to_vec_u8(&storage_footprint_to_ledger_footprint(&storage.footprint)?)?;
    pfc.with_cb(|cb| cb.set_result_value(&val_vec))?;
    pfc.with_cb(|cb| cb.set_result_footprint(&foot_vec))?;
    pfc.with_cb(|cb| cb.set_result_cpu_insns(cpu_insns))?;
    pfc.with_cb(|cb| cb.set_result_mem_bytes(mem_bytes))?;
    Ok(())
}
