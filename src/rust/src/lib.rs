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

    // If success is false, the only thing that may be populated is
    // diagnostic_events. The rest of the fields should be ignored.
    struct InvokeHostFunctionOutput {
        success: bool,
        result_value: RustBuf,
        contract_events: Vec<RustBuf>,
        diagnostic_events: Vec<RustBuf>,
        modified_ledger_entries: Vec<RustBuf>,
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

    #[derive(Debug)]
    enum BridgeError {
        VersionNotYetSupported,
    }

    struct VersionStringPair {
        curr: String,
        next: String,
    }

    struct VersionNumPair {
        curr: u64,
        next: u64,
    }

    struct XDRHashesPair {
        curr: Vec<XDRFileHash>,
        next: Vec<XDRFileHash>,
    }

    // The extern "Rust" block declares rust stuff we're going to export to C++.
    #[namespace = "stellar::rust_bridge"]
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn get_xdr_hashes() -> XDRHashesPair;
        fn check_lockfile_has_expected_dep_trees();
        fn invoke_host_function(
            config_max_protocol: u32,
            enable_diagnostics: bool,
            hf_buf: &CxxBuf,
            footprint: &CxxBuf,
            source_account: &CxxBuf,
            contract_auth_entries: &Vec<CxxBuf>,
            ledger_info: CxxLedgerInfo,
            ledger_entries: &Vec<CxxBuf>,
        ) -> Result<InvokeHostFunctionOutput>;
        fn init_logging(maxLevel: LogLevel) -> Result<()>;

        // Accessors for test wasms, compiled into soroban-test-wasms crate.
        fn get_test_wasm_add_i32() -> Result<RustBuf>;
        fn get_test_wasm_contract_data() -> Result<RustBuf>;
        fn get_test_wasm_complex() -> Result<RustBuf>;

        // Return the rustc version used to build this binary.
        fn get_rustc_version() -> String;

        // Return the env cargo package versions used to build this binary.
        fn get_soroban_env_pkg_versions() -> VersionStringPair;

        // Return the env git versions used to build this binary.
        fn get_soroban_env_git_versions() -> VersionStringPair;

        // Return the env interface versions used to build this binary.
        fn get_soroban_env_interface_versions() -> VersionNumPair;

        // Return the rust XDR bindings cargo package versions used to build this binary.
        fn get_soroban_xdr_bindings_pkg_versions() -> VersionStringPair;

        // Return the rust XDR bindings git versions used to build this binary.
        fn get_soroban_xdr_bindings_git_versions() -> VersionStringPair;

        // Return the rust XDR bindings' input XDR definitions git versions used to build this binary.
        fn get_soroban_xdr_bindings_base_xdr_git_versions() -> VersionStringPair;

        // Return true if configured with cfg(feature="soroban-env-host-next")
        fn has_soroban_next() -> bool;
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
use std::str::FromStr;

use b64::{from_base64, to_base64};

// Accessors for test wasms, compiled into soroban-test-wasms crate.
pub(crate) fn get_test_wasm_add_i32() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::ADD_I32.iter().cloned().collect(),
    })
}
pub(crate) fn get_test_wasm_contract_data() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONTRACT_STORAGE
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_test_wasm_complex() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::COMPLEX.iter().cloned().collect(),
    })
}

use rust_bridge::CxxBuf;
use rust_bridge::CxxLedgerInfo;
use rust_bridge::InvokeHostFunctionOutput;
use rust_bridge::RustBuf;
use rust_bridge::VersionNumPair;
use rust_bridge::VersionStringPair;
use rust_bridge::XDRHashesPair;

mod log;
use crate::log::init_logging;

fn package_matches_hash(pkg: &cargo_lock::Package, hash: &str) -> bool {
    // Try comparing hash to hashes in either the package checksum or the source
    // precise field
    if let Some(cksum) = &pkg.checksum {
        if cksum.to_string() == hash {
            return true;
        }
    }
    if let Some(src) = &pkg.source {
        if let Some(precise) = src.precise() {
            if precise == hash {
                return true;
            }
        }
    }
    false
}

// We have at least one, but possibly two, copies of soroban compiled
// in to stellar-core. If we have two, there's a constant `NEXT_PROTO`
// that is used to identify ledgers that should run on `soroban_env_host_curr`;
// later ledgers will run on `soroban_env_host_next`.

#[path = "."]
mod soroban_curr {
    pub(crate) use soroban_env_host_curr as soroban_env_host;
    pub(crate) mod contract;
}

#[cfg(feature = "soroban-env-host-next")]
#[path = "."]
mod soroban_next {
    pub(crate) use soroban_env_host_next as soroban_env_host;
    pub(crate) mod contract;
}

#[cfg(feature = "soroban-env-host-next")]
pub fn has_soroban_next() -> bool {
    true
}

#[cfg(not(feature = "soroban-env-host-next"))]
pub fn has_soroban_next() -> bool {
    false
}

use cargo_lock::{dependency::graph::EdgeDirection, Lockfile};

fn check_lockfile_has_expected_dep_tree(
    lockfile: &Lockfile,
    curr_or_next: &str,
    package_hash: &str,
    expected: &str,
) {
    let pkg = lockfile
        .packages
        .iter()
        .find(|p| p.name.as_str() == "soroban-env-host" && package_matches_hash(p, package_hash))
        .expect("locating host package in Cargo.lock");

    let tree = lockfile
        .dependency_tree()
        .expect("calculating global dep tree of Cargo.lock");

    let node = tree.nodes()[&pkg.into()];

    let mut tree_buf = Vec::new();
    tree.render(&mut tree_buf, node, EdgeDirection::Outgoing, true)
        .expect("rendering dep tree");

    let tree_str = String::from_utf8_lossy(&tree_buf);
    if tree_str != expected {
        eprintln!(
            "Expected '{}' host dependency tree (in host-dep-tree-{}.txt):",
            curr_or_next, curr_or_next
        );
        eprintln!("---\n{}\n---", expected);
        eprintln!(
            "Found '{}' host dependency tree (in Cargo.lock):",
            curr_or_next
        );
        eprintln!("---\n{}\n---", tree_str);
        panic!("Unexpected '{}' host dependency tree", curr_or_next);
    }
}

// This function performs a crude dynamic check that the contents of Cargo.lock
// against-which the current binary was compiled specified _exactly_ the same
// host dep trees that are stored (redundantly, graphically) in the files
// host-dep-tree-curr.txt and (if applicable) host-dep-tree-next.txt.
//
// The contents of all these files are compiled-in to the binary as static
// strings. Any discrepancy between the logical content of Cargo.lock and the
// derived dep tree(s) will cause the program to abort on startup.
//
// The point of this check is twofold: to catch cases where the developer
// accidentally bumps dependencies (which cargo does fairly easily), and also to
// make crystal clear when doing a commit that intentionally bumps dependencies
// which of the _dependency tree(s)_ is being affected, and how.
pub fn check_lockfile_has_expected_dep_trees() {
    static CARGO_LOCK_FILE_CONTENT: &'static str = include_str!("../../../Cargo.lock");

    static EXPECTED_HOST_DEP_TREE_CURR: &'static str = include_str!("host-dep-tree-curr.txt");
    #[cfg(feature = "soroban-env-host-next")]
    static EXPECTED_HOST_DEP_TREE_NEXT: &'static str = include_str!("host-dep-tree-next.txt");

    let lockfile = Lockfile::from_str(CARGO_LOCK_FILE_CONTENT)
        .expect("parsing compiled-in Cargo.lock file content");

    check_lockfile_has_expected_dep_tree(
        &lockfile,
        "curr",
        soroban_env_host_curr::VERSION.rev,
        EXPECTED_HOST_DEP_TREE_CURR,
    );
    #[cfg(feature = "soroban-env-host-next")]
    check_lockfile_has_expected_dep_tree(
        &lockfile,
        "next",
        soroban_env_host_next::VERSION.rev,
        EXPECTED_HOST_DEP_TREE_NEXT,
    );
}

// The remainder of the file is implementations of functions
// declared above in the rust_bridge module.

fn get_rustc_version() -> String {
    rustc_simple_version::RUSTC_VERSION.to_string()
}

fn get_soroban_env_pkg_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.pkg.to_string(),
        #[cfg(feature = "soroban-env-host-next")]
        next: soroban_next::soroban_env_host::VERSION.pkg.to_string(),
        #[cfg(not(feature = "soroban-env-host-next"))]
        next: "".to_string(),
    }
}

fn get_soroban_env_git_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.rev.to_string(),
        #[cfg(feature = "soroban-env-host-next")]
        next: soroban_next::soroban_env_host::VERSION.rev.to_string(),
        #[cfg(not(feature = "soroban-env-host-next"))]
        next: "".to_string(),
    }
}

fn get_soroban_env_interface_versions() -> VersionNumPair {
    VersionNumPair {
        curr: soroban_curr::soroban_env_host::VERSION.interface,
        #[cfg(feature = "soroban-env-host-next")]
        next: soroban_next::soroban_env_host::VERSION.interface,
        #[cfg(not(feature = "soroban-env-host-next"))]
        next: 0,
    }
}

fn get_soroban_xdr_bindings_pkg_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.xdr.pkg.to_string(),
        #[cfg(feature = "soroban-env-host-next")]
        next: soroban_next::soroban_env_host::VERSION.xdr.pkg.to_string(),
        #[cfg(not(feature = "soroban-env-host-next"))]
        next: "".to_string(),
    }
}

fn get_soroban_xdr_bindings_git_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.xdr.rev.to_string(),
        #[cfg(feature = "soroban-env-host-next")]
        next: soroban_next::soroban_env_host::VERSION.xdr.rev.to_string(),
        #[cfg(not(feature = "soroban-env-host-next"))]
        next: "".to_string(),
    }
}

fn get_soroban_xdr_bindings_base_xdr_git_versions() -> VersionStringPair {
    let curr = match soroban_curr::soroban_env_host::VERSION.xdr.xdr {
        "next" => soroban_curr::soroban_env_host::VERSION
            .xdr
            .xdr_next
            .to_string(),
        "curr" => soroban_curr::soroban_env_host::VERSION
            .xdr
            .xdr_curr
            .to_string(),
        _ => "unknown configuration".to_string(),
    };
    #[cfg(feature = "soroban-env-host-next")]
    let next = match soroban_next::soroban_env_host::VERSION.xdr.xdr {
        "next" => soroban_next::soroban_env_host::VERSION
            .xdr
            .xdr_next
            .to_string(),
        "curr" => soroban_next::soroban_env_host::VERSION
            .xdr
            .xdr_curr
            .to_string(),
        _ => "unknown configuration".to_string(),
    };
    #[cfg(not(feature = "soroban-env-host-next"))]
    let next = "".to_string();
    VersionStringPair { curr, next }
}

impl std::fmt::Display for rust_bridge::BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}
impl std::error::Error for rust_bridge::BridgeError {}

pub(crate) fn get_xdr_hashes() -> XDRHashesPair {
    let curr = soroban_curr::contract::get_xdr_hashes();
    #[cfg(feature = "soroban-env-host-next")]
    let next = soroban_next::contract::get_xdr_hashes();
    #[cfg(not(feature = "soroban-env-host-next"))]
    let next = vec![];
    XDRHashesPair { curr, next }
}

pub(crate) fn invoke_host_function(
    config_max_protocol: u32,
    enable_diagnostics: bool,
    hf_buf: &CxxBuf,
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    contract_auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>> {
    if ledger_info.protocol_version > config_max_protocol {
        return Err(Box::new(soroban_curr::contract::CoreHostError::General(
            "unsupported protocol",
        )));
    }
    #[cfg(feature = "soroban-env-host-next")]
    {
        if ledger_info.protocol_version == config_max_protocol {
            return soroban_next::contract::invoke_host_function(
                enable_diagnostics,
                hf_buf,
                footprint_buf,
                source_account_buf,
                contract_auth_entries,
                ledger_info,
                ledger_entries,
            );
        }
    }
    soroban_curr::contract::invoke_host_function(
        enable_diagnostics,
        hf_buf,
        footprint_buf,
        source_account_buf,
        contract_auth_entries,
        ledger_info,
        ledger_entries,
    )
}
