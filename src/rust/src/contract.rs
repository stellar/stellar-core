// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{log::partition::TX, rust_bridge::XDRBuf};
use log::info;
use std::io::Cursor;

use im_rc::OrdMap;
use std::error::Error;
use stellar_contract_env_host::{
    storage::{self, AccessType, Key, Storage},
    xdr,
    xdr::{
        ContractDataEntry, LedgerEntry, LedgerEntryData, LedgerKey, ReadXdr, ScVal, ScVec,
        WriteXdr,
    },
    Host, HostError,
};

/// Helper for [`build_storage_footprint_from_xdr`] that inserts a copy of some
/// [`AccessType`] `ty` into a [`storage::Footprint`] for every [`LedgerKey`] in
/// `keys`.
fn populate_access_map_with_contract_data_keys(
    access: &mut OrdMap<Key, AccessType>,
    keys: Vec<LedgerKey>,
    ty: AccessType,
) -> Result<(), HostError> {
    for lk in keys {
        match lk {
            LedgerKey::ContractData(xdr::LedgerKeyContractData { contract_id, key }) => {
                let sk = Key { contract_id, key };
                access.insert(sk, ty.clone());
            }
            _ => return Err(HostError::General("unexpected ledger key type").into()),
        }
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

    populate_access_map_with_contract_data_keys(
        &mut access,
        read_only.to_vec(),
        AccessType::ReadOnly,
    )?;
    populate_access_map_with_contract_data_keys(
        &mut access,
        read_write.to_vec(),
        AccessType::ReadWrite,
    )?;
    Ok(storage::Footprint(access))
}

/// Deserializes a sequence of [`xdr::LedgerEntry`] structures from a vector of
/// [`XDRBuf`] buffers, inserts them into an [`OrdMap`] with entries
/// additionally keyed by the provided contract ID, checks that the entries
/// match the provided [`storage::Footprint`], and returns the constructed map.
fn build_storage_map_from_xdr_ledger_entries(
    footprint: &storage::Footprint,
    ledger_entries: &Vec<XDRBuf>,
) -> Result<OrdMap<Key, Option<ScVal>>, HostError> {
    let mut map = OrdMap::new();
    for buf in ledger_entries {
        let le = LedgerEntry::read_xdr(&mut Cursor::new(buf.data.as_slice()))?;
        match le.data {
            LedgerEntryData::ContractData(ContractDataEntry {
                key,
                val,
                contract_id,
            }) => {
                let sk = Key { contract_id, key };
                if !footprint.0.contains_key(&sk) {
                    return Err(HostError::General("ledger entry not found in footprint").into());
                }
                map.insert(sk.clone(), Some(val));
            }
            _ => return Err(HostError::General("unexpected ledger entry type").into()),
        }
    }
    for k in footprint.0.keys() {
        if !map.contains_key(k) {
            return Err(HostError::General("ledger entry not found for footprint entry").into());
        }
    }
    Ok(map)
}
