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

    struct PreflightHostFunctionOutput {
        result_value: RustBuf,
        contract_events: Vec<RustBuf>,
        diagnostic_events: Vec<RustBuf>,
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

    #[derive(Debug)]
    enum BridgeError {
        VersionNotYetSupported,
    }

    struct VersionStringPair {
        lo: String,
        hi: String,
    }

    struct VersionNumPair {
        lo: u64,
        hi: u64,
    }

    struct XDRHashesPair {
        lo: Vec<XDRFileHash>,
        hi: Vec<XDRFileHash>,
    }

    // The extern "Rust" block declares rust stuff we're going to export to C++.
    #[namespace = "stellar::rust_bridge"]
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn get_xdr_hashes() -> XDRHashesPair;
        fn check_lockfile_has_expected_dep_trees();
        fn invoke_host_function(
            host_logic_version: u32,
            enable_diagnostics: bool,
            hf_buf: &CxxBuf,
            footprint: &CxxBuf,
            source_account: &CxxBuf,
            contract_auth_entries: &Vec<CxxBuf>,
            ledger_info: CxxLedgerInfo,
            ledger_entries: &Vec<CxxBuf>,
        ) -> Result<InvokeHostFunctionOutput>;
        fn preflight_host_function(
            host_logic_version: u32,
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

        // Return the host logic versions designating the lo and hi host environments in this binary.
        fn get_soroban_host_logic_versions() -> VersionNumPair;
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

// old:

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

use cxx::CxxVector;
use cxx::UniquePtr;
use rust_bridge::CxxBuf;
use rust_bridge::CxxLedgerInfo;
use rust_bridge::InvokeHostFunctionOutput;
use rust_bridge::PreflightCallbacks;
use rust_bridge::PreflightHostFunctionOutput;
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

// We retain _two copies_ of the soroban_env_host crate, and all of its
// dependencies, compiled into stellar-core at all times. We refer to these two
// versions as 'lo' and 'hi' and there is a strict process for upgrading them,
// described in the document docs/versioning-soroban.md

#[path = "."]
mod lo {
    pub(crate) use soroban_env_host_lo as soroban_env_host;
    pub(crate) const LOGIC_VERSION: u32 = 1;
    pub(crate) mod contract;
}

#[path = "."]
mod hi {
    pub(crate) use soroban_env_host_hi as soroban_env_host;
    pub(crate) const LOGIC_VERSION: u32 = super::lo::LOGIC_VERSION + 1;
    pub(crate) mod contract;
}

use cargo_lock::{dependency::graph::EdgeDirection, Lockfile};

fn check_lockfile_has_expected_dep_tree(
    lockfile: &Lockfile,
    hi_or_lo: &str,
    logic_version: u32,
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

    let tree_str = format!(
        "Logic version: {}\nDependency tree:\n{}",
        logic_version,
        String::from_utf8_lossy(&tree_buf)
    );

    if tree_str != expected {
        eprintln!(
            "Expected '{}' host dependency tree (in host-dep-tree-{}.txt):",
            hi_or_lo, hi_or_lo
        );
        eprintln!("---\n{}\n---", expected);
        eprintln!("Found '{}' host dependency tree (in Cargo.lock):", hi_or_lo);
        eprintln!("---\n{}\n---", tree_str);
        panic!("Unexpected '{}' host dependency tree", hi_or_lo);
    }
}

// This function performs a crude dynamic check that the contents of Cargo.lock
// against-which the current binary was compiled specified _exactly_ the same
// host dep trees that are stored (redundantly, graphically) in the files
// host-dep-tree-lo.txt and host-dep-tree.hi, and moreover that the logic
// version numbers we have assigned in the 'lo' and 'hi' modules below are
// unchanged from those stored redundantly in the dep tree files.
//
// The contents of all 3 files are compiled-in to the binary as static strings.
// Any discrepancy between the logical content of Cargo.lock and the two derived
// dep trees will cause the program to abort on startup.
//
// The point of this check is twofold: to catch cases where the developer
// accidentally bumps dependencies (which cargo does fairly easily), and also to
// make crystal clear when doing a commit that intentionally bumps dependencies
// which of the two _overlapping dependency trees_ is being affected, and how.
pub fn check_lockfile_has_expected_dep_trees() {
    static CARGO_LOCK_FILE_CONTENT: &'static str = include_str!("../../../Cargo.lock");

    static EXPECTED_HOST_DEP_TREE_LO: &'static str = include_str!("host-dep-tree-lo.txt");
    static EXPECTED_HOST_DEP_TREE_HI: &'static str = include_str!("host-dep-tree-hi.txt");

    let lockfile = Lockfile::from_str(CARGO_LOCK_FILE_CONTENT)
        .expect("parsing compiled-in Cargo.lock file content");

    check_lockfile_has_expected_dep_tree(
        &lockfile,
        "lo",
        self::lo::LOGIC_VERSION,
        self::lo::soroban_env_host::VERSION.rev,
        EXPECTED_HOST_DEP_TREE_LO,
    );
    check_lockfile_has_expected_dep_tree(
        &lockfile,
        "hi",
        self::hi::LOGIC_VERSION,
        self::hi::soroban_env_host::VERSION.rev,
        EXPECTED_HOST_DEP_TREE_HI,
    );
}

// This is a trait that abstracts the translation from host logic version numbers
// (which are assigned by stellar-core, as the embedder of the host) and host
// `LegacyEpoch` values (which are added to the host as needed for replay). It also
// abstracts over the fact that a version of the host might not have a definition
// for `LegacyEpoch` yet at all, nor a method on `Host` to install one. This trait
// is called from _within_ the body of contract::{invoke,preflight}_host_function,
// but has a different implementation for each of {hi,lo}::soroban_env_host::Host.
trait SetHostLogicVersion {
    fn set_logic_version(&self, version: u32);
}

impl SetHostLogicVersion for lo::soroban_env_host::Host {
    fn set_logic_version(&self, version: u32) {
        // Does nothing, both because the current version of `lo` has no
        // definition of `LegacyEpoch`, and also in general we should never
        // run `lo` on any host logic version other than `lo::LOGIC_VERSION`
        // if we're doing things correctly, and that should never require a
        // `LegacyEpoch` adjustment in the `Host`.
        assert!(version == lo::LOGIC_VERSION);
    }
}

impl SetHostLogicVersion for hi::soroban_env_host::Host {
    fn set_logic_version(&self, version: u32) {
        use hi::soroban_env_host::LegacyEpoch;
        let epoch = match version {
            _ => LegacyEpoch::Current,
        };
        self.set_legacy_epoch(epoch)
    }
}

// The remainder of the file is implementations of functions
// declared above in the rust_bridge module.

fn get_rustc_version() -> String {
    rustc_simple_version::RUSTC_VERSION.to_string()
}

fn get_soroban_env_pkg_versions() -> VersionStringPair {
    VersionStringPair {
        lo: lo::soroban_env_host::VERSION.pkg.to_string(),
        hi: hi::soroban_env_host::VERSION.pkg.to_string(),
    }
}

fn get_soroban_env_git_versions() -> VersionStringPair {
    VersionStringPair {
        lo: lo::soroban_env_host::VERSION.rev.to_string(),
        hi: hi::soroban_env_host::VERSION.rev.to_string(),
    }
}

fn get_soroban_env_interface_versions() -> VersionNumPair {
    VersionNumPair {
        lo: lo::soroban_env_host::VERSION.interface,
        hi: hi::soroban_env_host::VERSION.interface,
    }
}

fn get_soroban_xdr_bindings_pkg_versions() -> VersionStringPair {
    VersionStringPair {
        lo: lo::soroban_env_host::VERSION.xdr.pkg.to_string(),
        hi: hi::soroban_env_host::VERSION.xdr.pkg.to_string(),
    }
}

fn get_soroban_xdr_bindings_git_versions() -> VersionStringPair {
    VersionStringPair {
        lo: lo::soroban_env_host::VERSION.xdr.rev.to_string(),
        hi: hi::soroban_env_host::VERSION.xdr.rev.to_string(),
    }
}

fn get_soroban_xdr_bindings_base_xdr_git_versions() -> VersionStringPair {
    let lo = match lo::soroban_env_host::VERSION.xdr.xdr {
        "next" => lo::soroban_env_host::VERSION.xdr.xdr_next.to_string(),
        "curr" => lo::soroban_env_host::VERSION.xdr.xdr_curr.to_string(),
        _ => "unknown configuration".to_string(),
    };
    let hi = match hi::soroban_env_host::VERSION.xdr.xdr {
        "next" => hi::soroban_env_host::VERSION.xdr.xdr_next.to_string(),
        "curr" => hi::soroban_env_host::VERSION.xdr.xdr_curr.to_string(),
        _ => "unknown configuration".to_string(),
    };
    VersionStringPair { lo, hi }
}

fn get_soroban_host_logic_versions() -> VersionNumPair {
    VersionNumPair {
        lo: self::lo::LOGIC_VERSION as u64,
        hi: self::hi::LOGIC_VERSION as u64,
    }
}

impl std::fmt::Display for rust_bridge::BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}
impl std::error::Error for rust_bridge::BridgeError {}

pub(crate) fn get_xdr_hashes() -> XDRHashesPair {
    let lo = lo::contract::get_xdr_hashes();
    let hi = hi::contract::get_xdr_hashes();
    XDRHashesPair { lo, hi }
}

pub(crate) fn invoke_host_function(
    host_logic_version: u32,
    enable_diagnostics: bool,
    hf_buf: &CxxBuf,
    footprint_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    contract_auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>> {
    if host_logic_version > hi::LOGIC_VERSION {
        Err(Box::new(rust_bridge::BridgeError::VersionNotYetSupported))
    } else if host_logic_version == lo::LOGIC_VERSION {
        lo::contract::invoke_host_function(
            host_logic_version,
            enable_diagnostics,
            hf_buf,
            footprint_buf,
            source_account_buf,
            contract_auth_entries,
            ledger_info,
            ledger_entries,
        )
    } else {
        // We will execute `hi` for host logic versions equal to
        // `hi::LOGIC_VERSION` _and_ versions strictly less than
        // `lo::LOGIC_VERSION`. The former run with no modification, the latter
        // potentially with a `LegacyEpoch` to activate backward-compatibility
        // legacy code paths.
        hi::contract::invoke_host_function(
            host_logic_version,
            enable_diagnostics,
            hf_buf,
            footprint_buf,
            source_account_buf,
            contract_auth_entries,
            ledger_info,
            ledger_entries,
        )
    }
}

pub(crate) fn preflight_host_function(
    host_logic_version: u32,
    hf_buf: &CxxVector<u8>,
    source_account_buf: &CxxVector<u8>,
    ledger_info: CxxLedgerInfo,
    cb: UniquePtr<PreflightCallbacks>,
) -> Result<PreflightHostFunctionOutput, Box<dyn std::error::Error>> {
    if host_logic_version > hi::LOGIC_VERSION {
        Err(Box::new(rust_bridge::BridgeError::VersionNotYetSupported))
    } else if host_logic_version == lo::LOGIC_VERSION {
        lo::contract::preflight_host_function(
            host_logic_version,
            hf_buf,
            source_account_buf,
            ledger_info,
            cb,
        )
    } else {
        // We will execute `hi` for host logic versions equal to
        // `hi::LOGIC_VERSION` _and_ versions strictly less than
        // `lo::LOGIC_VERSION`. The former run with no modification, the latter
        // potentially with a `LegacyEpoch` to activate backward-compatibility
        // legacy code paths.
        hi::contract::preflight_host_function(
            host_logic_version,
            hf_buf,
            source_account_buf,
            ledger_info,
            cb,
        )
    }
}
