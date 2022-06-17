// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{Bytes, XDRBuf},
};
use log::info;
use std::io::Cursor;

use im_rc::OrdMap;
use std::error::Error;
use stellar_contract_env_host::{
    storage::{self, AccessType, Storage},
    xdr,
    xdr::{
        HostFunction, LedgerEntry, LedgerEntryData, LedgerKey, LedgerKeyAccount,
        LedgerKeyContractData, LedgerKeyTrustLine, ReadXdr, ScVec, WriteXdr,
    },
    Host, HostError,
};

/// Helper for [`build_storage_footprint_from_xdr`] that inserts a copy of some
/// [`AccessType`] `ty` into a [`storage::Footprint`] for every [`LedgerKey`] in
/// `keys`.
fn populate_access_map(
    access: &mut OrdMap<LedgerKey, AccessType>,
    keys: Vec<LedgerKey>,
    ty: AccessType,
) -> Result<(), HostError> {
    for lk in keys {
        match lk {
            LedgerKey::Account(_) | LedgerKey::Trustline(_) | LedgerKey::ContractData(_) => (),
            _ => return Err(HostError::General("unexpected ledger entry type")),
        };
        access.insert(lk, ty.clone());
    }
    Ok(())
}

/// Deserializes an [`xdr::LedgerFootprint`] from the provided [`XDRBuf`], converts
/// its entries to an [`OrdMap`], and returns a [`storage::Footprint`]
/// containing that map.
fn build_storage_footprint_from_xdr(footprint: &XDRBuf) -> Result<storage::Footprint, HostError> {
    let xdr::LedgerFootprint {
        read_only,
        read_write,
    } = xdr::LedgerFootprint::read_xdr(&mut Cursor::new(footprint.data.as_slice()))?;
    let mut access = OrdMap::new();

    populate_access_map(&mut access, read_only.to_vec(), AccessType::ReadOnly)?;
    populate_access_map(&mut access, read_write.to_vec(), AccessType::ReadWrite)?;
    Ok(storage::Footprint(access))
}

fn ledger_entry_to_ledger_key(le: &LedgerEntry) -> Result<LedgerKey, HostError> {
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
        _ => Err(HostError::General("unexpected ledger key")),
    }
}

/// Deserializes a sequence of [`xdr::LedgerEntry`] structures from a vector of
/// [`XDRBuf`] buffers, inserts them into an [`OrdMap`] with entries
/// additionally keyed by the provided contract ID, checks that the entries
/// match the provided [`storage::Footprint`], and returns the constructed map.
fn build_storage_map_from_xdr_ledger_entries(
    footprint: &storage::Footprint,
    ledger_entries: &Vec<XDRBuf>,
) -> Result<OrdMap<LedgerKey, Option<LedgerEntry>>, HostError> {
    let mut map = OrdMap::new();
    for buf in ledger_entries {
        let le = LedgerEntry::read_xdr(&mut Cursor::new(buf.data.as_slice()))?;
        let key = ledger_entry_to_ledger_key(&le)?;
        if !footprint.0.contains_key(&key) {
            return Err(HostError::General("ledger entry not found in footprint").into());
        }
        map.insert(key, Some(le));
    }
    for k in footprint.0.keys() {
        if !map.contains_key(k) {
            map.insert(k.clone(), None);
        }
    }
    Ok(map)
}

/// Iterates over the storage map and serializes the read-write ledger entries
/// back to XDR.
fn build_xdr_ledger_entries_from_storage_map(
    footprint: &storage::Footprint,
    storage_map: &OrdMap<LedgerKey, Option<LedgerEntry>>,
) -> Result<Vec<Bytes>, HostError> {
    let mut res = Vec::new();
    for (lk, ole) in storage_map {
        let mut xdr_buf: Vec<u8> = Vec::new();
        match footprint.0.get(lk) {
            Some(AccessType::ReadOnly) => (),
            Some(AccessType::ReadWrite) => {
                if let Some(le) = ole {
                    le.write_xdr(&mut Cursor::new(&mut xdr_buf))?;
                    res.push(Bytes { vec: xdr_buf });
                }
            }
            None => return Err(HostError::General("ledger entry not in footprint")),
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
    let hf = HostFunction::read_xdr(&mut Cursor::new(hf_buf.data.as_slice()))?;
    let args = ScVec::read_xdr(&mut Cursor::new(args_buf.data.as_slice()))?;

    let footprint = build_storage_footprint_from_xdr(footprint_buf)?;
    let map = build_storage_map_from_xdr_ledger_entries(&footprint, ledger_entries)?;

    let storage = Storage::with_enforcing_footprint_and_map(footprint, map);
    let mut host = Host::with_storage(storage);

    match hf {
        HostFunction::Call => {
            info!(target: TX, "Invoking host function 'Call'");
            host.invoke_function(hf, args)?;
        }
        HostFunction::CreateContract => {
            info!(target: TX, "Invoking host function 'CreateContract'");
            todo!();
        }
    };

    let storage = host
        .try_recover_storage()
        .map_err(|_h| HostError::General("could not get storage from host"))?;
    Ok(build_xdr_ledger_entries_from_storage_map(
        &storage.footprint,
        &storage.map,
    )?)
}
