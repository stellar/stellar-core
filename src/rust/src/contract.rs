// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{log::partition::TX, rust_bridge::XDRBuf};
use log::info;
use std::io::Cursor;

use cxx::CxxString;
use im_rc::OrdMap;
use std::error::Error;
use stellar_contract_env_host::{
    storage::{self, AccessType, Key, Storage},
    xdr,
    xdr::{
        ContractDataEntry, Hash, LedgerEntry, LedgerEntryData, LedgerKey, ReadXdr, ScObject,
        ScStatic, ScVal, ScVec, WriteXdr,
    },
    Host, HostError, Vm,
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

/// Looks up an [`ScObject`] un the provided map under the ledger key
/// [`ScStatic::LedgerKeyContractCodeWasm`] and returns a copy of its binary
/// data, which should be WASM bytecode.
fn extract_contract_wasm_from_storage_map(
    contract_id: &Hash,
    map: &OrdMap<Key, Option<ScVal>>,
) -> Result<Vec<u8>, HostError> {
    let wasm_key = Key {
        contract_id: contract_id.clone(),
        key: ScVal::Static(ScStatic::LedgerKeyContractCodeWasm),
    };
    let wasm = match map.get(&wasm_key) {
        Some(Some(ScVal::Object(Some(ScObject::Binary(blob))))) => blob.clone(),
        Some(_) => {
            return Err(HostError::General(
                "unexpected value type for LEDGER_KEY_CONTRACT_CODE_WASM",
            )
            .into())
        }
        None => {
            return Err(
                HostError::General("missing value for LEDGER_KEY_CONTRACT_CODE_WASM").into(),
            )
        }
    };
    Ok(wasm.to_vec())
}

/// Deserializes an [`xdr::Hash`] contract ID, an [`ScVec`] XDR object of
/// arguments, an [`xdr::LedgerFootprint`] and a sequence of [`xdr::LedgerEntry`]
/// entries containing all the data the contract intends to read. Then loads
/// some WASM bytecode out of the provided ledger entries (keyed under
/// [`ScStatic::LedgerKeyContractCodeWasm`]), instantiates a [`Host`] and [`Vm`]
/// with the provided WASM, invokes the requested function in the WASM, and
/// serializes an [`xdr::ScVal`] back into a return value.
pub(crate) fn invoke_contract(
    contract_id: &XDRBuf,
    func: &CxxString,
    args: &XDRBuf,
    footprint: &XDRBuf,
    ledger_entries: &Vec<XDRBuf>,
) -> Result<Vec<u8>, Box<dyn Error>> {
    let contract_id = Hash::read_xdr(&mut Cursor::new(contract_id.data.as_slice()))?;
    let arg_scvals = ScVec::read_xdr(&mut Cursor::new(args.data.as_slice()))?;

    let footprint = build_storage_footprint_from_xdr(footprint)?;
    let map = build_storage_map_from_xdr_ledger_entries(&footprint, ledger_entries)?;
    let wasm = extract_contract_wasm_from_storage_map(&contract_id, &map)?;

    let func_str = func.to_str()?;

    let storage = Storage::with_enforcing_footprint_and_map(footprint, map);
    let mut host = Host::with_storage(storage);
    let vm = Vm::new(&host, contract_id, wasm.as_slice())?;

    info!(target: TX, "Invoking contract function '{}'", func);
    let res = vm.invoke_function(&mut host, func_str, &arg_scvals)?;

    let mut ret_xdr_buf: Vec<u8> = Vec::new();
    res.write_xdr(&mut Cursor::new(&mut ret_xdr_buf))?;
    Ok(ret_xdr_buf)
}
