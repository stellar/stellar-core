// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#![crate_type = "staticlib"]
#![allow(non_snake_case)]

#[cfg(feature = "tracy")]
macro_rules! tracy_span {
    () => {
        tracy_client::span!()
    };
    ($name:expr) => {
        tracy_client::span!($name)
    };
}

#[cfg(not(feature = "tracy"))]
macro_rules! tracy_span {
    () => {
        ()
    };
    ($name:expr) => {
        ()
    };
}

pub(crate) mod common;
pub(crate) mod soroban_proto_all;

mod b64;
mod i128;
mod log;
mod soroban_invoke;
mod soroban_module_cache;
mod soroban_test_wasm;

#[cfg(feature = "testutils")]
mod soroban_test_extra_protocol;

use soroban_module_cache::SorobanModuleCache;

mod bridge;
use bridge::rust_bridge;
use rust_bridge::BridgeError;
use rust_bridge::CxxBuf;
use rust_bridge::CxxFeeConfiguration;
use rust_bridge::CxxLedgerEntryRentChange;
use rust_bridge::CxxLedgerInfo;
use rust_bridge::CxxRentFeeConfiguration;

use rust_bridge::CxxI128;
use rust_bridge::CxxTransactionResources;
use rust_bridge::CxxWriteFeeConfiguration;
use rust_bridge::FeePair;
use rust_bridge::InvokeHostFunctionOutput;
use rust_bridge::RustBuf;
use rust_bridge::SorobanVersionInfo;
