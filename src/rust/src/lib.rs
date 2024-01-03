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

    // Result of invoking a host function.
    // When `success` is `false`, the function has failed. The diagnostic events
    // and metering data will be populated, but result value and effects won't
    // be populated.
    struct InvokeHostFunctionOutput {
        success: bool,
        // In case if `success` is `false` indicates whether the host has
        // failed with an internal error.
        // We don't otherwise observe the error codes, but internal errors are
        // something that should never happen, so it's important to be able
        // to act on them in Core.
        is_internal_error: bool,
        // Diagnostic information concerning the host function execution.
        diagnostic_events: Vec<RustBuf>,
        cpu_insns: u64,
        mem_bytes: u64,
        time_nsecs: u64,
        cpu_insns_excluding_vm_instantiation: u64,
        time_nsecs_excluding_vm_instantiation: u64,

        // Effects of the invocation that are only populated in case of success.
        result_value: RustBuf,
        contract_events: Vec<RustBuf>,
        modified_ledger_entries: Vec<RustBuf>,
        rent_fee: i64,
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
        pub memory_limit: u32,
        pub min_temp_entry_ttl: u32,
        pub min_persistent_entry_ttl: u32,
        pub max_entry_ttl: u32,
        pub cpu_cost_params: CxxBuf,
        pub mem_cost_params: CxxBuf,
    }

    #[derive(Debug)]
    enum BridgeError {
        VersionNotYetSupported,
    }

    struct VersionStringPair {
        curr: String,
        prev: String,
    }

    struct VersionNumPair {
        curr: u32,
        prev: u32,
    }

    struct XDRHashesPair {
        curr: Vec<XDRFileHash>,
        prev: Vec<XDRFileHash>,
    }

    struct CxxTransactionResources {
        instructions: u32,
        read_entries: u32,
        write_entries: u32,
        read_bytes: u32,
        write_bytes: u32,
        contract_events_size_bytes: u32,
        transaction_size_bytes: u32,
    }

    struct CxxFeeConfiguration {
        fee_per_instruction_increment: i64,
        fee_per_read_entry: i64,
        fee_per_write_entry: i64,
        fee_per_read_1kb: i64,
        fee_per_write_1kb: i64,
        fee_per_historical_1kb: i64,
        fee_per_contract_event_1kb: i64,
        fee_per_transaction_size_1kb: i64,
    }

    struct CxxLedgerEntryRentChange {
        is_persistent: bool,
        old_size_bytes: u32,
        new_size_bytes: u32,
        old_live_until_ledger: u32,
        new_live_until_ledger: u32,
    }

    struct CxxRentFeeConfiguration {
        fee_per_write_1kb: i64,
        fee_per_write_entry: i64,
        persistent_rent_rate_denominator: i64,
        temporary_rent_rate_denominator: i64,
    }

    struct CxxWriteFeeConfiguration {
        bucket_list_target_size_bytes: i64,
        write_fee_1kb_bucket_list_low: i64,
        write_fee_1kb_bucket_list_high: i64,
        bucket_list_write_fee_growth_factor: u32,
    }

    struct FeePair {
        non_refundable_fee: i64,
        refundable_fee: i64,
    }

    // The extern "Rust" block declares rust stuff we're going to export to C++.
    #[namespace = "stellar::rust_bridge"]
    extern "Rust" {
        fn start_tracy();
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn get_xdr_hashes() -> XDRHashesPair;
        fn check_lockfile_has_expected_dep_trees(curr_max_protocol_version: u32);
        fn invoke_host_function(
            config_max_protocol: u32,
            enable_diagnostics: bool,
            instruction_limit: u32,
            hf_buf: &CxxBuf,
            resources: &CxxBuf,
            source_account: &CxxBuf,
            auth_entries: &Vec<CxxBuf>,
            ledger_info: CxxLedgerInfo,
            ledger_entries: &Vec<CxxBuf>,
            ttl_entries: &Vec<CxxBuf>,
            base_prng_seed: &CxxBuf,
            rent_fee_configuration: CxxRentFeeConfiguration,
        ) -> Result<InvokeHostFunctionOutput>;
        fn init_logging(maxLevel: LogLevel) -> Result<()>;

        // Accessors for test wasms, compiled into soroban-test-wasms crate.
        fn get_test_wasm_add_i32() -> Result<RustBuf>;
        fn get_test_wasm_contract_data() -> Result<RustBuf>;
        fn get_test_wasm_complex() -> Result<RustBuf>;
        fn get_test_wasm_loadgen() -> Result<RustBuf>;
        fn get_test_wasm_err() -> Result<RustBuf>;
        fn get_test_contract_sac_transfer() -> Result<RustBuf>;
        fn get_write_bytes() -> Result<RustBuf>;

        fn get_hostile_large_val_wasm() -> Result<RustBuf>;

        fn get_auth_wasm() -> Result<RustBuf>;

        fn get_custom_account_wasm() -> Result<RustBuf>;

        // Utility functions for generating wasms using soroban-synth-wasm.
        fn get_random_wasm(size: usize, seed: u64) -> Result<RustBuf>;

        // Return the rustc version used to build this binary.
        fn get_rustc_version() -> String;

        // Return the env cargo package versions used to build this binary.
        fn get_soroban_env_pkg_versions() -> VersionStringPair;

        // Return the env git versions used to build this binary.
        fn get_soroban_env_git_versions() -> VersionStringPair;

        // Return the env protocol versions used to build this binary.
        fn get_soroban_env_ledger_protocol_versions() -> VersionNumPair;

        // Return the env pre-release versions used to build this binary.
        fn get_soroban_env_pre_release_versions() -> VersionNumPair;

        // Return the rust XDR bindings cargo package versions used to build this binary.
        fn get_soroban_xdr_bindings_pkg_versions() -> VersionStringPair;

        // Return the rust XDR bindings git versions used to build this binary.
        fn get_soroban_xdr_bindings_git_versions() -> VersionStringPair;

        // Return the rust XDR bindings' input XDR definitions git versions used to build this binary.
        fn get_soroban_xdr_bindings_base_xdr_git_versions() -> VersionStringPair;

        // Return true if configured with cfg(feature="soroban-env-host-prev")
        fn compiled_with_soroban_prev() -> bool;

        // Computes the resource fee given the transaction resource consumption
        // and network configuration.
        fn compute_transaction_resource_fee(
            config_max_protocol: u32,
            protocol_version: u32,
            tx_resources: CxxTransactionResources,
            fee_config: CxxFeeConfiguration,
        ) -> Result<FeePair>;

        // Computes the write fee per 1kb written to the ledger given the
        // current bucket list size and network configuration.
        fn compute_write_fee_per_1kb(
            config_max_protocol: u32,
            protocol_version: u32,
            bucket_list_size: i64,
            fee_config: CxxWriteFeeConfiguration,
        ) -> Result<i64>;

        // Computes the rent fee given the ledger entry changes and network
        // configuration.
        fn compute_rent_fee(
            config_max_protocol: u32,
            protocol_version: u32,
            changed_entries: &Vec<CxxLedgerEntryRentChange>,
            fee_config: CxxRentFeeConfiguration,
            current_ledger_seq: u32,
        ) -> Result<i64>;
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

impl From<Vec<u8>> for RustBuf {
    fn from(value: Vec<u8>) -> Self {
        Self { data: value }
    }
}

impl AsRef<[u8]> for CxxBuf {
    fn as_ref(&self) -> &[u8] {
        self.data.as_slice()
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

pub(crate) fn get_test_wasm_err() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::ERR.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_wasm_loadgen() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::LOADGEN.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_contract_sac_transfer() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONTRACT_SAC_TRANSFER_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_write_bytes() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::WRITE_BYTES.iter().cloned().collect(),
    })
}

pub(crate) fn get_hostile_large_val_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::HOSTILE_LARGE_VALUE
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_auth_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::AUTH_TEST_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_custom_account_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::SIMPLE_ACCOUNT_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

fn get_random_wasm(size: usize, seed: u64) -> Result<RustBuf, Box<dyn std::error::Error>> {
    use rand::{rngs::StdRng, RngCore, SeedableRng};
    use soroban_synth_wasm::*;
    let mut fe = ModEmitter::default().func(Arity(0), 0);

    // Generate a very exciting wasm that pushes and drops random numbers.
    //
    // We want to generate a random i64 number that is exactly 6 bytes when
    // encoded as LEB128, so that that number plus two 1-byte opcodes gets us to
    // 8 bytes.
    //
    // LEB128 encodes 7 bits at a time into each byte. So a 6 byte output will
    // encode 6 * 7 = 42 bits of input. Or it would if it was unsigned; signed
    // LEB128 needs 1 bit from the MSB for sign-indication, so it's actually 41
    // bits.
    //
    // We want to set the 41st bit of that to 1 to guarantee it's always set,
    // and then the low 40 bits we will assign random data to.
    let mut prng = StdRng::seed_from_u64(seed);
    let n_loops = size / 8;
    for _ in 0..n_loops {
        let mut i = prng.next_u64();
        // Cut the random number down to 41 bits.
        i &= 0x0000_01ff_ffff_ffff;
        // Ensure the number has its 41st bit set.
        i |= 0x0000_0100_0000_0000;
        // Emit a 1-byte opcode + 6 byte LEB128 = 7 bytes.
        fe.i64_const(i as i64);
        // Emit 1 more byte of opcode, making 8.
        fe.drop();
    }
    // Push a return value.
    fe.i64_const(0);
    fe.ret();
    Ok(RustBuf {
        data: fe.finish_and_export("test").finish(),
    })
}

#[test]
fn test_get_random_wasm() {
    // HEADERSZ varies a bit as we fiddle with the definition of the wasm
    // synthesis system we want to check that it hasn't grown unreasonable and
    // we're _basically_ building something of the requested size, or at least
    // right order of magnitude.
    const HEADERSZ: usize = 150;
    for i in 1..10 {
        let wasm = get_random_wasm(1000 * i, i as u64).unwrap();
        assert!(wasm.data.len() < 1000 * i + HEADERSZ);
    }
}

use rust_bridge::CxxBuf;
use rust_bridge::CxxFeeConfiguration;
use rust_bridge::CxxLedgerEntryRentChange;
use rust_bridge::CxxLedgerInfo;
use rust_bridge::CxxRentFeeConfiguration;
use rust_bridge::CxxTransactionResources;
use rust_bridge::CxxWriteFeeConfiguration;
use rust_bridge::FeePair;
use rust_bridge::InvokeHostFunctionOutput;
use rust_bridge::RustBuf;
use rust_bridge::VersionNumPair;
use rust_bridge::VersionStringPair;
use rust_bridge::XDRHashesPair;

mod log;

use crate::log::init_logging;

// We have at least one, but possibly two, copies of soroban compiled
// in to stellar-core. If we have two, ledgers that are exactly one
// protocol _before_ the current (max-supported) protocol will run on
// the `prev` copy of soroban. All others will run on the `curr` copy.
// See `invoke_host_function` below.

#[path = "."]
mod soroban_curr {
    pub(crate) use soroban_env_host_curr as soroban_env_host;

    pub(crate) mod contract;
}

#[cfg(feature = "soroban-env-host-prev")]
#[path = "."]
mod soroban_prev {
    pub(crate) use soroban_env_host_prev as soroban_env_host;

    pub(crate) mod contract;
}

#[cfg(feature = "soroban-env-host-prev")]
pub fn compiled_with_soroban_prev() -> bool {
    true
}

#[cfg(not(feature = "soroban-env-host-prev"))]
pub fn compiled_with_soroban_prev() -> bool {
    false
}

use cargo_lock::{dependency::graph::EdgeDirection, Lockfile};

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

fn check_lockfile_has_expected_dep_tree(
    stellar_core_proto_version: u32,
    soroban_host_interface_version: u64,
    lockfile: &Lockfile,
    curr_or_prev: &str,
    package_hash: &str,
    expected: &str,
) {
    use soroban_curr::soroban_env_host::meta::{
        get_ledger_protocol_version, get_pre_release_version,
    };
    let soroban_host_proto_version = get_ledger_protocol_version(soroban_host_interface_version);

    if cfg!(feature = "core-vnext") {
        // In a stellar-core "vnext" build, core's protocol is set to 1 more
        // than the network's current max-supported version, to prototype new
        // work that we don't want to get released to the network by accident,
        // during a point release.
        //
        // Soroban has no corresponding concept of a "vnext build" so we just
        // use a weak version check here, and let core's protocol get ahead of
        // soroban's in this type of build.
        if stellar_core_proto_version < soroban_host_proto_version {
            panic!(
                "stellar-core \"{}\" protocol is {}, does not match soroban host \"{}\" protocol {}",
                curr_or_prev, stellar_core_proto_version, curr_or_prev, soroban_host_proto_version
            );
        }
    } else {
        // In non-vnext core builds, we're more strict about the versions
        // matching.
        if stellar_core_proto_version != soroban_host_proto_version {
            panic!(
                "stellar-core \"{}\" protocol is {}, does not match soroban host \"{}\" protocol {}",
                curr_or_prev, stellar_core_proto_version, curr_or_prev, soroban_host_proto_version
            );
        }
    }

    let pkg = lockfile
        .packages
        .iter()
        .find(|p| p.name.as_str() == "soroban-env-host" && package_matches_hash(p, package_hash))
        .expect("locating host package in Cargo.lock");

    if !cfg!(feature = "core-vnext") {
        let soroban_host_pre_release_version =
            get_pre_release_version(soroban_host_interface_version);
        if soroban_host_pre_release_version != 0 && pkg.version.pre.is_empty() {
            panic!("soroban interface version indicates pre-release {} but package version is {}, with empty prerelease component",
                soroban_host_pre_release_version, pkg.version)
        }

        if pkg.version.major == 0 || !pkg.version.pre.is_empty() {
            eprintln!(
                "Warning: soroban-env-host-{} is running a pre-release version {}",
                curr_or_prev, pkg.version
            );
        } else if pkg.version.major != stellar_core_proto_version as u64 {
            panic!(
                "soroban-env-host-{} version {} major version {} does not match expected protocol version {}",
                curr_or_prev, pkg.version, pkg.version.major, stellar_core_proto_version
            )
        }
    }

    let tree = lockfile
        .dependency_tree()
        .expect("calculating global dep tree of Cargo.lock");

    let node = tree.nodes()[&pkg.into()];

    let mut tree_buf = Vec::new();
    tree.render(&mut tree_buf, node, EdgeDirection::Outgoing, true)
        .expect("rendering dep tree");

    let tree_str = String::from_utf8_lossy(&tree_buf);
    // Normalize line endings to support Windows builds.
    if tree_str.replace("\r\n", "\n") != expected.replace("\r\n", "\n") {
        eprintln!(
            "Expected '{}' host dependency tree (in host-dep-tree-{}.txt):",
            curr_or_prev, curr_or_prev
        );
        eprintln!("---\n{}---", expected);
        eprintln!(
            "Found '{}' host dependency tree (in Cargo.lock):",
            curr_or_prev
        );
        eprintln!("---\n{}---", tree_str);
        panic!("Unexpected '{}' host dependency tree", curr_or_prev);
    }
}

// This function performs a crude dynamic check that the contents of Cargo.lock
// against-which the current binary was compiled specified _exactly_ the same
// host dep trees that are stored (redundantly, graphically) in the files
// host-dep-tree-curr.txt and (if applicable) host-dep-tree-prev.txt.
//
// The contents of all these files are compiled-in to the binary as static
// strings. Any discrepancy between the logical content of Cargo.lock and the
// derived dep tree(s) will cause the program to abort on startup.
//
// The point of this check is twofold: to catch cases where the developer
// accidentally bumps dependencies (which cargo does fairly easily), and also to
// make crystal clear when doing a commit that intentionally bumps dependencies
// which of the _dependency tree(s)_ is being affected, and how.
//
// The check additionally checks that the major version number of soroban that
// is compiled-in matches its max supported protocol number and that that
// is the same as stellar-core's max supported protocol number.
pub fn check_lockfile_has_expected_dep_trees(curr_max_protocol_version: u32) {
    static CARGO_LOCK_FILE_CONTENT: &'static str = include_str!("../../../Cargo.lock");

    static EXPECTED_HOST_DEP_TREE_CURR: &'static str = include_str!("host-dep-tree-curr.txt");
    #[cfg(feature = "soroban-env-host-prev")]
    static EXPECTED_HOST_DEP_TREE_PREV: &'static str = include_str!("host-dep-tree-prev.txt");

    let lockfile = Lockfile::from_str(CARGO_LOCK_FILE_CONTENT)
        .expect("parsing compiled-in Cargo.lock file content");

    check_lockfile_has_expected_dep_tree(
        curr_max_protocol_version,
        soroban_env_host_curr::meta::INTERFACE_VERSION,
        &lockfile,
        "curr",
        soroban_env_host_curr::VERSION.rev,
        EXPECTED_HOST_DEP_TREE_CURR,
    );
    #[cfg(feature = "soroban-env-host-prev")]
    check_lockfile_has_expected_dep_tree(
        curr_max_protocol_version - 1,
        soroban_env_host_prev::meta::INTERFACE_VERSION,
        &lockfile,
        "prev",
        soroban_env_host_prev::VERSION.rev,
        EXPECTED_HOST_DEP_TREE_PREV,
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
        #[cfg(feature = "soroban-env-host-prev")]
        prev: soroban_prev::soroban_env_host::VERSION.pkg.to_string(),
        #[cfg(not(feature = "soroban-env-host-prev"))]
        prev: "".to_string(),
    }
}

fn get_soroban_env_git_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.rev.to_string(),
        #[cfg(feature = "soroban-env-host-prev")]
        prev: soroban_prev::soroban_env_host::VERSION.rev.to_string(),
        #[cfg(not(feature = "soroban-env-host-prev"))]
        prev: "".to_string(),
    }
}

fn get_soroban_env_ledger_protocol_versions() -> VersionNumPair {
    use curr_host::meta::get_ledger_protocol_version;
    use soroban_curr::soroban_env_host as curr_host;
    #[cfg(feature = "soroban-env-host-prev")]
    use soroban_prev::soroban_env_host as prev_host;
    VersionNumPair {
        curr: get_ledger_protocol_version(curr_host::VERSION.interface),
        #[cfg(feature = "soroban-env-host-prev")]
        prev: get_ledger_protocol_version(prev_host::VERSION.interface),
        #[cfg(not(feature = "soroban-env-host-prev"))]
        prev: 0,
    }
}

fn get_soroban_env_pre_release_versions() -> VersionNumPair {
    use curr_host::meta::get_pre_release_version;
    use soroban_curr::soroban_env_host as curr_host;
    #[cfg(feature = "soroban-env-host-prev")]
    use soroban_prev::soroban_env_host as prev_host;
    VersionNumPair {
        curr: get_pre_release_version(curr_host::VERSION.interface),
        #[cfg(feature = "soroban-env-host-prev")]
        prev: get_pre_release_version(prev_host::VERSION.interface),
        #[cfg(not(feature = "soroban-env-host-prev"))]
        prev: 0,
    }
}

fn get_soroban_xdr_bindings_pkg_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.xdr.pkg.to_string(),
        #[cfg(feature = "soroban-env-host-prev")]
        prev: soroban_prev::soroban_env_host::VERSION.xdr.pkg.to_string(),
        #[cfg(not(feature = "soroban-env-host-prev"))]
        prev: "".to_string(),
    }
}

fn get_soroban_xdr_bindings_git_versions() -> VersionStringPair {
    VersionStringPair {
        curr: soroban_curr::soroban_env_host::VERSION.xdr.rev.to_string(),
        #[cfg(feature = "soroban-env-host-prev")]
        prev: soroban_prev::soroban_env_host::VERSION.xdr.rev.to_string(),
        #[cfg(not(feature = "soroban-env-host-prev"))]
        prev: "".to_string(),
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
    #[cfg(feature = "soroban-env-host-prev")]
    let prev = match soroban_prev::soroban_env_host::VERSION.xdr.xdr {
        "next" => soroban_prev::soroban_env_host::VERSION
            .xdr
            .xdr_next
            .to_string(),
        "curr" => soroban_prev::soroban_env_host::VERSION
            .xdr
            .xdr_curr
            .to_string(),
        _ => "unknown configuration".to_string(),
    };
    #[cfg(not(feature = "soroban-env-host-prev"))]
    let prev = "".to_string();
    VersionStringPair { curr, prev }
}

impl std::fmt::Display for rust_bridge::BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl std::error::Error for rust_bridge::BridgeError {}

pub(crate) fn get_xdr_hashes() -> XDRHashesPair {
    let curr = soroban_curr::contract::get_xdr_hashes();
    #[cfg(feature = "soroban-env-host-prev")]
    let prev = soroban_prev::contract::get_xdr_hashes();
    #[cfg(not(feature = "soroban-env-host-prev"))]
    let prev = vec![];
    XDRHashesPair { curr, prev }
}

pub(crate) fn invoke_host_function(
    config_max_protocol: u32,
    enable_diagnostics: bool,
    instruction_limit: u32,
    hf_buf: &CxxBuf,
    resources_buf: &CxxBuf,
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    ttl_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
    rent_fee_configuration: CxxRentFeeConfiguration,
) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>> {
    if ledger_info.protocol_version > config_max_protocol {
        return Err(Box::new(soroban_curr::contract::CoreHostError::General(
            "unsupported protocol",
        )));
    }
    #[cfg(feature = "soroban-env-host-prev")]
    {
        if ledger_info.protocol_version == config_max_protocol - 1 {
            return soroban_prev::contract::invoke_host_function(
                enable_diagnostics,
                instruction_limit,
                hf_buf,
                resources_buf,
                source_account_buf,
                auth_entries,
                ledger_info,
                ledger_entries,
                ttl_entries,
                base_prng_seed,
                rent_fee_configuration,
            );
        }
    }
    soroban_curr::contract::invoke_host_function(
        enable_diagnostics,
        instruction_limit,
        hf_buf,
        resources_buf,
        source_account_buf,
        auth_entries,
        ledger_info,
        ledger_entries,
        ttl_entries,
        base_prng_seed,
        rent_fee_configuration,
    )
}

pub(crate) fn compute_transaction_resource_fee(
    config_max_protocol: u32,
    protocol_version: u32,
    tx_resources: CxxTransactionResources,
    fee_config: CxxFeeConfiguration,
) -> Result<FeePair, Box<dyn std::error::Error>> {
    if protocol_version > config_max_protocol {
        return Err(Box::new(soroban_curr::contract::CoreHostError::General(
            "unsupported protocol",
        )));
    }
    #[cfg(feature = "soroban-env-host-prev")]
    {
        if protocol_version == config_max_protocol - 1 {
            return Ok(soroban_prev::contract::compute_transaction_resource_fee(
                tx_resources,
                fee_config,
            ));
        }
    }
    Ok(soroban_curr::contract::compute_transaction_resource_fee(
        tx_resources,
        fee_config,
    ))
}

pub(crate) fn compute_rent_fee(
    config_max_protocol: u32,
    protocol_version: u32,
    changed_entries: &Vec<CxxLedgerEntryRentChange>,
    fee_config: CxxRentFeeConfiguration,
    current_ledger_seq: u32,
) -> Result<i64, Box<dyn std::error::Error>> {
    if protocol_version > config_max_protocol {
        return Err(Box::new(soroban_curr::contract::CoreHostError::General(
            "unsupported protocol",
        )));
    }
    #[cfg(feature = "soroban-env-host-prev")]
    {
        if protocol_version == config_max_protocol - 1 {
            return Ok(soroban_prev::contract::compute_rent_fee(
                changed_entries,
                fee_config,
                current_ledger_seq,
            ));
        }
    }
    Ok(soroban_curr::contract::compute_rent_fee(
        changed_entries,
        fee_config,
        current_ledger_seq,
    ))
}

pub(crate) fn compute_write_fee_per_1kb(
    config_max_protocol: u32,
    protocol_version: u32,
    bucket_list_size: i64,
    fee_config: CxxWriteFeeConfiguration,
) -> Result<i64, Box<dyn std::error::Error>> {
    if protocol_version > config_max_protocol {
        return Err(Box::new(soroban_curr::contract::CoreHostError::General(
            "unsupported protocol",
        )));
    }
    #[cfg(feature = "soroban-env-host-prev")]
    {
        if protocol_version == config_max_protocol - 1 {
            return Ok(soroban_prev::contract::compute_write_fee_per_1kb(
                bucket_list_size,
                fee_config,
            ));
        }
    }
    Ok(soroban_curr::contract::compute_write_fee_per_1kb(
        bucket_list_size,
        fee_config,
    ))
}

fn start_tracy() {
    #[cfg(feature = "tracy")]
    tracy_client::Client::start();
    #[cfg(not(feature = "tracy"))]
    panic!("called start_tracy from non-cfg(feature=\"tracy\") build")
}
