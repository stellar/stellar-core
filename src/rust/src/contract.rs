// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::log::partition::TX;
use log::info;
use std::io::Cursor;
use tracy_client::{span, Client};

use cxx::{CxxString, CxxVector};
use std::error::Error;
use stellar_contract_env_host::{
    xdr::{ReadXdr, ScVec, WriteXdr},
    Host, VM,
};

/// Deserialize an SCVec XDR object of SCVal arguments from the C++ side of the
/// bridge, instantiate a Host and VM with the provided WASM, invoke the
/// requested function in the WASM, and serialize an SCVal back into a return
/// value.
pub(crate) fn invoke_contract(
    wasm: &CxxVector<u8>,
    func: &CxxString,
    args: &CxxVector<u8>,
) -> Result<Vec<u8>, Box<dyn Error>> {
    let client = Client::start();
    let _span = span!("invoke_contract");
    let arg_scvals = ScVec::read_xdr(&mut Cursor::new(args.as_slice()))?;
    let func_str = func.to_str()?;

    let mut host = Host::default();
    let vm = VM::new(&host, wasm.as_slice())?;

    info!(target: TX, "Invoking contract function '{}'", func);
    let _span2 = span!("invoke_function");
    let res = vm.invoke_function(&mut host, func_str, &arg_scvals)?;

    let mut ret_xdr_buf: Vec<u8> = Vec::new();
    res.write_xdr(&mut Cursor::new(&mut ret_xdr_buf))?;
    Ok(ret_xdr_buf)
}
