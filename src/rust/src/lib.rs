// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#![crate_type = "staticlib"]
#![allow(non_snake_case)]

// The cxx::bridge attribute says that everything in mod rust_bridge is
// interpreted by cxx.rs.
#[cxx::bridge]
mod rust_bridge {

    // We want to pass around vectors of XDR buffers (CxxVector<CxxVector<...>>) or similar,
    // but cxx.rs has some limits around this (eg. https://github.com/dtolnay/cxx/issues/671)
    // So far this is the best approximate mechanism found.
    struct XDRBuf {
        data: UniquePtr<CxxVector<u8>>,
    }

    // LogLevel declares to cxx.rs a shared type that both Rust and C+++ will
    // understand.
    #[namespace = "stellar"]
    enum LogLevel {
        #[allow(unused)]
        LVL_FATAL = 0,
        LVL_ERROR = 1,
        LVL_WARNING = 2,
        LVL_INFO = 3,
        LVL_DEBUG = 4,
        LVL_TRACE = 5,
    }

    // The extern "Rust" block declares rust stuff we're going to export to C++.
    #[namespace = "stellar::rust_bridge"]
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn invoke_contract(
            contract_id: &XDRBuf,
            func: &CxxString,
            args: &XDRBuf,
            footprint: &XDRBuf,
            ledger_entries: &Vec<XDRBuf>,
        ) -> Result<Vec<u8>>;
        fn init_logging(maxLevel: LogLevel) -> Result<()>;
    }

    // And the extern "C++" block declares C++ stuff we're going to import to
    // Rust.
    #[namespace = "stellar"]
    unsafe extern "C++" {
        include!("rust/CppShims.h");
        // This declares (and asserts) that the external C++ definition of
        // stellar::LogLevel must match (in size and discriminant values) the
        // shared type declared above.
        type LogLevel;
        fn shim_isLogLevelAtLeast(partition: &CxxString, level: LogLevel) -> bool;
        fn shim_logAtPartitionAndLevel(partition: &CxxString, level: LogLevel, msg: &CxxString);
    }
}

// Then we import various implementations to this module, for export through the bridge.
mod b64;
use b64::{from_base64, to_base64};

mod contract;
use contract::invoke_contract;

mod log;
use crate::log::init_logging;
