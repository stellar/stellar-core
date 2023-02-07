// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#![crate_type = "staticlib"]
#![allow(non_snake_case)]

// The cxx::bridge attribute says that everything in mod rust_bridge is
// interpreted by cxx.rs.
#[cxx::bridge]
mod rust_bridge {

    // When we want to pass owned data _from_ C++, we typically want to pass it
    // as a C++-allocated std::vector<uint8_t>, because that's most-compatible
    // with all the C++ functions we're likely to be using to build it.
    //
    // Unfortunately cxx.rs has some limits around this (eg.
    // https://github.com/dtolnay/cxx/issues/671) So we need to embed it in a
    // struct that, itself, holds a unique_ptr. It's a bit silly but seems
    // harmless enough.
    struct CxxBuf {
        data: UniquePtr<CxxVector<u8>>,
    }

    // When we want to return owned data _from_ Rust, we typically want to do
    // the opposite: allocate on the Rust side as a Vec<u8> and then let the C++
    // side parse the data out of it and then drop it.
    struct RustBuf {
        data: Vec<u8>,
    }

    // We return these from get_xdr_hashes below.
    struct XDRFileHash {
        file: String,
        hash: String,
    }

    struct InvokeHostFunctionOutput {
        result_value: RustBuf,
        contract_events: Vec<RustBuf>,
        modified_ledger_entries: Vec<RustBuf>,
        cpu_insns: u64,
        mem_bytes: u64,
    }

    struct PreflightHostFunctionOutput {
        result_value: RustBuf,
        contract_events: Vec<RustBuf>,
        storage_footprint: RustBuf,
        cpu_insns: u64,
        mem_bytes: u64,
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

    struct CxxLedgerInfo {
        pub protocol_version: u32,
        pub sequence_number: u32,
        pub timestamp: u64,
        pub network_id: Vec<u8>,
        pub base_reserve: u32,
    }

    // The extern "Rust" block declares rust stuff we're going to export to C++.
    #[namespace = "stellar::rust_bridge"]
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn get_xdr_hashes() -> Vec<XDRFileHash>;
        fn invoke_host_function(
            hf_buf: &CxxBuf,
            footprint: &CxxBuf,
            source_account: &CxxBuf,
            contract_auth_entries: &Vec<CxxBuf>,
            ledger_info: CxxLedgerInfo,
            ledger_entries: &Vec<CxxBuf>,
        ) -> Result<InvokeHostFunctionOutput>;
        fn preflight_host_function(
            hf_buf: &CxxVector<u8>,
            source_account: &CxxVector<u8>,
            ledger_info: CxxLedgerInfo,
            cb: UniquePtr<PreflightCallbacks>,
        ) -> Result<PreflightHostFunctionOutput>;
        fn init_logging(maxLevel: LogLevel) -> Result<()>;

        // Accessors for test wasms, compiled into soroban-test-wasms crate.
        fn get_test_wasm_add_i32() -> Result<RustBuf>;
        fn get_test_wasm_contract_data() -> Result<RustBuf>;
        fn get_test_wasm_complex() -> Result<RustBuf>;

        // Return the rustc version used to build this binary.
        fn get_rustc_version() -> String;

        // Return the env cargo package version used to build this binary.
        fn get_soroban_env_pkg_version() -> String;

        // Return the env git version used to build this binary.
        fn get_soroban_env_git_version() -> String;

        // Return the env interface version used to build this binary.
        fn get_soroban_env_interface_version() -> u64;

        // Return the rust XDR bindings cargo package version used to build this binary.
        fn get_soroban_xdr_bindings_pkg_version() -> String;

        // Return the rust XDR bindings git version used to build this binary.
        fn get_soroban_xdr_bindings_git_version() -> String;

        // Return the rust XDR bindings' input XDR definitions git version used to build this binary.
        fn get_soroban_xdr_bindings_base_xdr_git_version() -> String;
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
        fn get_ledger_entry(self: Pin<&mut PreflightCallbacks>, key: &Vec<u8>) -> Result<CxxBuf>;
        fn has_ledger_entry(self: Pin<&mut PreflightCallbacks>, key: &Vec<u8>) -> Result<bool>;
    }
}

// Then we import various implementations to this module, for export through the bridge.
mod b64;
use b64::{from_base64, to_base64};

mod contract;
use contract::get_test_wasm_add_i32;
use contract::get_test_wasm_complex;
use contract::get_test_wasm_contract_data;
use contract::get_xdr_hashes;
use contract::invoke_host_function;
use contract::preflight_host_function;

mod log;
use crate::log::init_logging;

fn get_rustc_version() -> String {
    rustc_simple_version::RUSTC_VERSION.to_string()
}

fn get_soroban_env_pkg_version() -> String {
    soroban_env_host::VERSION.pkg.to_string()
}

fn get_soroban_env_git_version() -> String {
    soroban_env_host::VERSION.rev.to_string()
}

fn get_soroban_env_interface_version() -> u64 {
    soroban_env_host::VERSION.interface
}

fn get_soroban_xdr_bindings_pkg_version() -> String {
    soroban_env_host::VERSION.xdr.pkg.to_string()
}

fn get_soroban_xdr_bindings_git_version() -> String {
    soroban_env_host::VERSION.xdr.rev.to_string()
}

fn get_soroban_xdr_bindings_base_xdr_git_version() -> String {
    soroban_env_host::VERSION.xdr.xdr.to_string()
}
