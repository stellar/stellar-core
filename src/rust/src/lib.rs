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

    struct Bytes {
        vec: Vec<u8>,
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
        fn invoke_host_function(
            hf_buf: &XDRBuf,
            args: &XDRBuf,
            footprint: &XDRBuf,
            ledger_entries: &Vec<XDRBuf>,
        ) -> Result<Vec<Bytes>>;
        fn preflight_host_function(
            hf_buf: &CxxVector<u8>,
            args: &CxxVector<u8>,
            cb: UniquePtr<PreflightCallbacks>,
        ) -> Result<()>;
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

        // This declares a type used by Rust to call back to C++ to access
        // a ledger snapshot and record other information related to a preflight
        // request.
        type PreflightCallbacks;
        fn get_ledger_entry(self: Pin<&mut PreflightCallbacks>, key: &Vec<u8>) -> Result<XDRBuf>;
        fn has_ledger_entry(self: Pin<&mut PreflightCallbacks>, key: &Vec<u8>) -> Result<bool>;
        // Since we're already passing a callback handle for the snapshot
        // access, we use this to convey all the structured return-values from
        // the preflight request as well.
        fn set_result_value(self: Pin<&mut PreflightCallbacks>, value: &Vec<u8>) -> Result<()>;
        fn set_result_footprint(
            self: Pin<&mut PreflightCallbacks>,
            footprint: &Vec<u8>,
        ) -> Result<()>;
        fn set_result_cpu_insns(self: Pin<&mut PreflightCallbacks>, cpu: u64) -> Result<()>;
        fn set_result_mem_bytes(self: Pin<&mut PreflightCallbacks>, mem: u64) -> Result<()>;
    }
}

// Then we import various implementations to this module, for export through the bridge.
mod b64;
use b64::{from_base64, to_base64};

mod contract;
use contract::invoke_host_function;
use contract::preflight_host_function;

mod log;
use crate::log::init_logging;
