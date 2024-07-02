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

    struct SorobanVersionInfo {
        pub env_max_proto: u32,
        pub env_pkg_ver: String,
        pub env_git_rev: String,
        pub env_pre_release_ver: u32,

        pub xdr_pkg_ver: String,
        pub xdr_git_rev: String,
        pub xdr_base_git_rev: String,
        pub xdr_file_hashes: Vec<XDRFileHash>,
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
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn check_sensible_soroban_config_for_protocol(core_max_proto: u32);
        fn invoke_host_function(
            config_max_protocol: u32,
            enable_diagnostics: bool,
            instruction_limit: u32,
            hf_buf: &CxxBuf,
            resources: CxxBuf,
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
        fn get_test_wasm_sum_i32() -> Result<RustBuf>;
        fn get_test_wasm_contract_data() -> Result<RustBuf>;
        fn get_test_wasm_complex() -> Result<RustBuf>;
        fn get_test_wasm_loadgen() -> Result<RustBuf>;
        fn get_test_wasm_err() -> Result<RustBuf>;
        fn get_test_contract_sac_transfer() -> Result<RustBuf>;
        fn get_write_bytes() -> Result<RustBuf>;
        fn get_invoke_contract_wasm() -> Result<RustBuf>;

        fn get_hostile_large_val_wasm() -> Result<RustBuf>;

        fn get_auth_wasm() -> Result<RustBuf>;

        fn get_no_arg_constructor_wasm() -> Result<RustBuf>;
        fn get_constructor_with_args_p21_wasm() -> Result<RustBuf>;
        fn get_constructor_with_args_p22_wasm() -> Result<RustBuf>;

        fn get_custom_account_wasm() -> Result<RustBuf>;

        // Utility functions for generating wasms using soroban-synth-wasm.
        fn get_random_wasm(size: usize, seed: u64) -> Result<RustBuf>;

        // Return the rustc version used to build this binary.
        fn get_rustc_version() -> String;

        // Return the soroban versions linked into this binary. Panics
        // if the protocol version is not supported.
        fn get_soroban_version_info(core_max_proto: u32) -> Vec<SorobanVersionInfo>;

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

        // Checks if a provided `TransactionEnvelope` XDR can be parsed in the
        // provided `protocol_version`.
        fn can_parse_transaction(
            config_max_protocol: u32,
            protocol_version: u32,
            xdr: &CxxBuf,
            depth_limit: u32,
        ) -> Result<bool>;
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
        #[cfg(not(test))]
        fn shim_isLogLevelAtLeast(partition: &CxxString, level: LogLevel) -> Result<bool>;
        #[cfg(not(test))]
        fn shim_logAtPartitionAndLevel(
            partition: &CxxString,
            level: LogLevel,
            msg: &CxxString,
        ) -> Result<()>;
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

impl CxxBuf {
    #[cfg(feature = "testutils")]
    fn replace_data_with(&mut self, slice: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
        if self.data.is_null() {
            return Err("CxxBuf::replace_data_with: data is null".into());
        }
        while self.data.len() > 0 {
            self.data.pin_mut().pop();
        }
        for byte in slice {
            self.data.pin_mut().push(*byte);
        }
        Ok(())
    }
}

// Then we import various implementations to this module, for export through the bridge.
mod b64;

use core::panic;

use b64::{from_base64, to_base64};

// Accessors for test wasms, compiled into soroban-test-wasms crate.
pub(crate) fn get_test_wasm_add_i32() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::ADD_I32.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_wasm_sum_i32() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::SUM_I32.iter().cloned().collect(),
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

fn get_no_arg_constructor_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::NO_ARGUMENT_CONSTRUCTOR_TEST_CONTRACT_P22
            .iter()
            .cloned()
            .collect(),
    })
}

fn get_constructor_with_args_p21_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONSTRUCTOR_TEST_CONTRACT_P21
            .iter()
            .cloned()
            .collect(),
    })
}

fn get_constructor_with_args_p22_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONSTRUCTOR_TEST_CONTRACT_P22
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

pub(crate) fn get_invoke_contract_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::INVOKE_CONTRACT
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
use rust_bridge::SorobanVersionInfo;

mod log;
#[cfg(feature = "testutils")]
use ::log::{info, warn};
use log::init_logging;
#[cfg(feature = "testutils")]
use log::partition::TX;

// We have multiple copies of soroban linked into stellar-core here. This is
// accomplished using an adaptor module -- contract.rs -- mounted multiple times
// into the same outer crate, inside different modules p21, p22, etc. each with
// its own local binding for the external crate soroban_env_host. The
// contract.rs module imports soroban_env_host from `super` which means each
// instance of it sees a different soroban. This is a bit of a hack and only
// works when the soroban versions all have a compatible _enough_ interface to
// all be called from "the same" contract.rs.
#[path = "."]
mod p23 {
    pub(crate) extern crate soroban_env_host_p23;
    pub(crate) use soroban_env_host_p23 as soroban_env_host;

    pub(crate) mod contract;

    // An adapter for some API breakage between p21 and p22.
    pub(crate) const fn get_version_pre_release(v: &soroban_env_host::Version) -> u32 {
        v.interface.pre_release
    }

    pub(crate) const fn get_version_protocol(v: &soroban_env_host::Version) -> u32 {
        // Temporarily hardcode the protocol version until we actually bump it
        // in the host library.
        23
    }
}

#[path = "."]
mod p22 {
    pub(crate) extern crate soroban_env_host_p22;
    pub(crate) use soroban_env_host_p22 as soroban_env_host;

    pub(crate) mod contract;

    // An adapter for some API breakage between p21 and p22.
    pub(crate) const fn get_version_pre_release(v: &soroban_env_host::Version) -> u32 {
        v.interface.pre_release
    }

    pub(crate) const fn get_version_protocol(v: &soroban_env_host::Version) -> u32 {
        v.interface.protocol
    }
}

#[path = "."]
mod p21 {
    pub(crate) extern crate soroban_env_host_p21;
    pub(crate) use soroban_env_host_p21 as soroban_env_host;

    pub(crate) mod contract;

    // An adapter for some API breakage between p21 and p22.
    pub(crate) const fn get_version_pre_release(v: &soroban_env_host::Version) -> u32 {
        soroban_env_host::meta::get_pre_release_version(v.interface)
    }

    pub(crate) const fn get_version_protocol(v: &soroban_env_host::Version) -> u32 {
        soroban_env_host::meta::get_ledger_protocol_version(v.interface)
    }
}

// We alias the latest soroban as soroban_curr to help reduce churn in code
// that's just always supposed to use the latest.
use p22 as soroban_curr;

// This is called on startup and does any initial internal dynamic checks.
pub fn check_sensible_soroban_config_for_protocol(core_max_proto: u32) {
    use itertools::Itertools;
    for (lo, hi) in HOST_MODULES.iter().tuple_windows() {
        assert!(
            lo.max_proto < hi.max_proto,
            "host modules are not in ascending order"
        );
    }
    assert!(HOST_MODULES.last().unwrap().max_proto >= core_max_proto);
}

// The remainder of the file is implementations of functions
// declared above in the rust_bridge module.

fn get_rustc_version() -> String {
    rustc_simple_version::RUSTC_VERSION.to_string()
}

fn get_soroban_version_info(core_max_proto: u32) -> Vec<SorobanVersionInfo> {
    let infos: Vec<SorobanVersionInfo> = HOST_MODULES
        .iter()
        .map(|f| (f.get_soroban_version_info)(core_max_proto))
        .collect();
    // This check should be safe to keep. The feature soroban-vnext is passed
    // through to soroban-env-host-p{NN}/next and so should enable protocol
    // support for the next version simultaneously in core and soroban; and we
    // should really never otherwise have core compiled with a protocol version
    // that soroban doesn't support.
    if infos
        .iter()
        .find(|i| i.env_max_proto >= core_max_proto)
        .is_none()
    {
        panic!(
            "no soroban host found supporting stellar-core protocol {}",
            core_max_proto
        );
    }
    infos
}

impl std::fmt::Display for rust_bridge::BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl std::error::Error for rust_bridge::BridgeError {}

// Rust does not support first-class modules. This means we cannot put multiple
// modules into an array and iterate over it switching between them by protocol
// number. Which is what we want to do! But as a workaround, we can copy
// everything we want _from_ each module into a struct, and work with the struct
// as a first-class value. This is what we do here.
struct HostModule {
    // Technically the `get_version_info` function returns the max_proto as
    // well, but we want to compute it separately so it can be baked into a
    // compile-time constant to minimize the cost of the protocol-based
    // dispatch. The struct returned from `get_version_info` contains a bunch of
    // dynamic strings, which is necessary due to cxx limitations.
    max_proto: u32,
    get_soroban_version_info: fn(u32) -> SorobanVersionInfo,
    invoke_host_function: fn(
        enable_diagnostics: bool,
        instruction_limit: u32,
        hf_buf: &CxxBuf,
        resources_buf: &CxxBuf,
        source_account_buf: &CxxBuf,
        auth_entries: &Vec<CxxBuf>,
        ledger_info: &CxxLedgerInfo,
        ledger_entries: &Vec<CxxBuf>,
        ttl_entries: &Vec<CxxBuf>,
        base_prng_seed: &CxxBuf,
        rent_fee_configuration: &CxxRentFeeConfiguration,
    ) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>>,
    compute_transaction_resource_fee:
        fn(tx_resources: CxxTransactionResources, fee_config: CxxFeeConfiguration) -> FeePair,
    compute_rent_fee: fn(
        changed_entries: &Vec<CxxLedgerEntryRentChange>,
        fee_config: CxxRentFeeConfiguration,
        current_ledger_seq: u32,
    ) -> i64,
    compute_write_fee_per_1kb:
        fn(bucket_list_size: i64, fee_config: CxxWriteFeeConfiguration) -> i64,
    can_parse_transaction: fn(&CxxBuf, depth_limit: u32) -> bool,
    #[cfg(feature = "testutils")]
    rustbuf_containing_scval_to_string: fn(&RustBuf) -> String,
    #[cfg(feature = "testutils")]
    rustbuf_containing_diagnostic_event_to_string: fn(&RustBuf) -> String,
}

macro_rules! proto_versioned_functions_for_module {
    ($module:ident) => {
        HostModule {
            max_proto: $module::contract::get_max_proto(),
            get_soroban_version_info: $module::contract::get_soroban_version_info,
            invoke_host_function: $module::contract::invoke_host_function,
            compute_transaction_resource_fee: $module::contract::compute_transaction_resource_fee,
            compute_rent_fee: $module::contract::compute_rent_fee,
            compute_write_fee_per_1kb: $module::contract::compute_write_fee_per_1kb,
            can_parse_transaction: $module::contract::can_parse_transaction,
            #[cfg(feature = "testutils")]
            rustbuf_containing_scval_to_string:
                $module::contract::rustbuf_containing_scval_to_string,
            #[cfg(feature = "testutils")]
            rustbuf_containing_diagnostic_event_to_string:
                $module::contract::rustbuf_containing_diagnostic_event_to_string,
        }
    };
}

// NB: this list should be in ascending order. Out of order will cause
// an assert to fail in the by-protocol-number lookup function below.
const HOST_MODULES: &'static [HostModule] = &[
    proto_versioned_functions_for_module!(p21),
    proto_versioned_functions_for_module!(p22),
    proto_versioned_functions_for_module!(p23),
];

fn get_host_module_for_protocol(
    config_max_protocol: u32,
    ledger_protocol_version: u32,
) -> Result<&'static HostModule, Box<dyn std::error::Error>> {
    if ledger_protocol_version > config_max_protocol {
        return Err(Box::new(soroban_curr::contract::CoreHostError::General(
            "protocol exceeds configured max",
        )));
    }
    // Each host's max protocol implies a min protocol for the _next_
    // host in the list. The first entry's min protocol is 0.
    let mut curr_min_proto = 0;
    for curr in HOST_MODULES.iter() {
        assert!(curr_min_proto <= curr.max_proto);
        if curr_min_proto <= ledger_protocol_version && ledger_protocol_version <= curr.max_proto {
            return Ok(curr);
        }
        curr_min_proto = curr.max_proto + 1;
    }
    Err(Box::new(soroban_curr::contract::CoreHostError::General(
        "unsupported protocol",
    )))
}

#[test]
fn protocol_dispatches_as_expected() {
    assert_eq!(get_host_module_for_protocol(20, 20).unwrap().max_proto, 21);
    assert_eq!(get_host_module_for_protocol(21, 21).unwrap().max_proto, 21);
    assert_eq!(get_host_module_for_protocol(22, 22).unwrap().max_proto, 22);
    assert_eq!(get_host_module_for_protocol(23, 23).unwrap().max_proto, 23);

    // No protocols past the max known.
    let last_proto = HOST_MODULES.last().unwrap().max_proto;
    assert!(get_host_module_for_protocol(last_proto + 1, last_proto + 1).is_err());

    // No ledger protocol has to be less than config max.
    assert!(get_host_module_for_protocol(20, 21).is_err());
}

pub(crate) fn invoke_host_function(
    config_max_protocol: u32,
    enable_diagnostics: bool,
    instruction_limit: u32,
    hf_buf: &CxxBuf,
    resources_buf: CxxBuf,
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    ttl_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
    rent_fee_configuration: CxxRentFeeConfiguration,
) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>> {
    let hm = get_host_module_for_protocol(config_max_protocol, ledger_info.protocol_version)?;
    let res = (hm.invoke_host_function)(
        enable_diagnostics,
        instruction_limit,
        hf_buf,
        &resources_buf,
        source_account_buf,
        auth_entries,
        &ledger_info,
        ledger_entries,
        ttl_entries,
        base_prng_seed,
        &rent_fee_configuration,
    );

    #[cfg(feature = "testutils")]
    test_extra_protocol::maybe_invoke_host_function_again_and_compare_outputs(
        &res,
        &hm,
        config_max_protocol,
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

    res
}

// This module contains helper code to assist in comparing two versions of
// soroban against one another by running the same transaction twice on
// different hosts, and comparing its output for divergence or changes to costs.
// All this functionality is gated by the "testutils" feature.
#[cfg(feature = "testutils")]
mod test_extra_protocol {

    use super::*;
    use std::hash::Hasher;
    use std::str::FromStr;

    pub(super) fn maybe_invoke_host_function_again_and_compare_outputs(
        res1: &Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>>,
        hm1: &HostModule,
        _config_max_protocol: u32,
        _enable_diagnostics: bool,
        mut instruction_limit: u32,
        hf_buf: &CxxBuf,
        mut resources_buf: CxxBuf,
        source_account_buf: &CxxBuf,
        auth_entries: &Vec<CxxBuf>,
        mut ledger_info: CxxLedgerInfo,
        ledger_entries: &Vec<CxxBuf>,
        ttl_entries: &Vec<CxxBuf>,
        base_prng_seed: &CxxBuf,
        rent_fee_configuration: CxxRentFeeConfiguration,
    ) {
        if let Ok(extra) = std::env::var("SOROBAN_TEST_EXTRA_PROTOCOL") {
            if let Ok(proto) = u32::from_str(&extra) {
                info!(target: TX, "comparing soroban host for protocol {} with {}", ledger_info.protocol_version, proto);
                if let Ok(hm2) = get_host_module_for_protocol(proto, proto) {
                    if let Err(e) =
                        modify_ledger_info_for_extra_test_execution(&mut ledger_info, proto)
                    {
                        warn!(target: TX, "modifying ledger info for protocol {} re-execution failed: {:?}", proto, e);
                        return;
                    }
                    if let Err(e) = modify_resources_for_extra_test_execution(
                        &mut instruction_limit,
                        &mut resources_buf,
                        proto,
                    ) {
                        warn!(target: TX, "modifying resources for protocol {} re-execution failed: {:?}", proto, e);
                        return;
                    }
                    let res2 = (hm2.invoke_host_function)(
                        /*enable_diagnostics=*/ true,
                        instruction_limit,
                        hf_buf,
                        &resources_buf,
                        source_account_buf,
                        auth_entries,
                        &ledger_info,
                        ledger_entries,
                        ttl_entries,
                        base_prng_seed,
                        &rent_fee_configuration,
                    );
                    if mostly_the_same_host_function_output(&res1, &res2) {
                        info!(target: TX, "{}", summarize_host_function_output(hm1, &res1));
                        info!(target: TX, "{}", summarize_host_function_output(hm2, &res2));
                    } else {
                        warn!(target: TX, "{}", summarize_host_function_output(hm1, &res1));
                        warn!(target: TX, "{}", summarize_host_function_output(hm2, &res2));
                    }
                } else {
                    warn!(target: TX, "SOROBAN_TEST_EXTRA_PROTOCOL={} not supported", proto);
                }
            } else {
                warn!(target: TX, "invalid protocol number in SOROBAN_TEST_EXTRA_PROTOCOL");
            }
        }
    }

    fn modify_resources_for_extra_test_execution(
        instruction_limit: &mut u32,
        resources_buf: &mut CxxBuf,
        new_protocol: u32,
    ) -> Result<(), Box<dyn std::error::Error>> {
        match new_protocol {
            22 => {
                use p22::contract::{inplace_modify_cxxbuf_encoded_type, xdr::SorobanResources};
                if let Ok(extra) = std::env::var("SOROBAN_TEST_CPU_BUDGET_FACTOR") {
                    if let Ok(factor) = u32::from_str(&extra) {
                        inplace_modify_cxxbuf_encoded_type::<SorobanResources>(
                            resources_buf,
                            |resources: &mut SorobanResources| {
                                info!(target: TX, "multiplying CPU budget for re-execution by {}: {} -> {} (and {} -> {} in limit)",
                                        factor, resources.instructions, resources.instructions * factor, *instruction_limit, *instruction_limit * factor);
                                resources.instructions *= factor;
                                *instruction_limit *= factor;
                                Ok(())
                            },
                        )?;
                    } else {
                        warn!(target: TX, "SOROBAN_TEST_CPU_BUDGET_FACTOR={} not valid", extra);
                    }
                }
            }
            _ => (),
        }
        Ok(())
    }

    fn modify_ledger_info_for_extra_test_execution(
        ledger_info: &mut CxxLedgerInfo,
        new_protocol: u32,
    ) -> Result<(), Box<dyn std::error::Error>> {
        // Here we need to simulate any upgrade that would be done in the ledger
        // info to migrate from the old protocol to the new one. This is somewhat
        // protocol-specific so we just write it by hand.

        // At very least, we always need to upgrade the protocol version in the
        // ledger info.
        ledger_info.protocol_version = new_protocol;

        match new_protocol {
            // At present no adjustments need to be made, only new costs exist
            // in p22, not changes to existing ones.
            //
            // FIXME: any changes to cost types should be centralized and pulled
            // from the same location, having multiple copies like this is bad,
            // we already have a bug open on the problem occurring between
            // budget defaults in soroban and upgrades.
            //
            // See: https://github.com/stellar/stellar-core/issues/4496
            _ => (),
        }

        Ok(())
    }

    fn hash_rustbuf(buf: &RustBuf) -> u16 {
        use std::hash::Hash;
        let mut hasher = std::hash::DefaultHasher::new();
        buf.data.hash(&mut hasher);
        hasher.finish() as u16
    }
    fn hash_rustbufs(bufs: &Vec<RustBuf>) -> u16 {
        use std::hash::Hash;
        let mut hasher = std::hash::DefaultHasher::new();
        for buf in bufs.iter() {
            buf.data.hash(&mut hasher);
        }
        hasher.finish() as u16
    }
    fn mostly_the_same_host_function_output(
        res: &Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>>,
        res2: &Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>>,
    ) -> bool {
        match (res, res2) {
            (Ok(res), Ok(res2)) => {
                res.success == res2.success
                    && hash_rustbuf(&res.result_value) == hash_rustbuf(&res2.result_value)
                    && hash_rustbufs(&res.contract_events) == hash_rustbufs(&res2.contract_events)
                    && hash_rustbufs(&res.modified_ledger_entries)
                        == hash_rustbufs(&res2.modified_ledger_entries)
                    && res.rent_fee == res2.rent_fee
            }
            (Err(e), Err(e2)) => format!("{:?}", e) == format!("{:?}", e2),
            _ => false,
        }
    }
    fn summarize_host_function_output(
        hm: &HostModule,
        res: &Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>>,
    ) -> String {
        match res {
            Ok(res) if res.success => format!(
                "proto={}, ok/succ, res={:x}/{}, events={:x}, entries={:x}, rent={}, cpu={}, mem={}, nsec={}",
                hm.max_proto,
                hash_rustbuf(&res.result_value),
                (hm.rustbuf_containing_scval_to_string)(&res.result_value),
                hash_rustbufs(&res.contract_events),
                hash_rustbufs(&res.modified_ledger_entries),
                res.rent_fee,
                res.cpu_insns,
                res.mem_bytes,
                res.time_nsecs
            ),
            Ok(res) => format!(
                "proto={}, ok/fail, cpu={}, mem={}, nsec={}, diag={:?}",
                hm.max_proto,
                res.cpu_insns,
                res.mem_bytes,
                res.time_nsecs,
                res.diagnostic_events.iter().map(|d|
                    (hm.rustbuf_containing_diagnostic_event_to_string)(d)).collect::<Vec<String>>()
            ),
            Err(e) => format!("proto={}, error={:?}", hm.max_proto, e),
        }
    }
}

pub(crate) fn compute_transaction_resource_fee(
    config_max_protocol: u32,
    protocol_version: u32,
    tx_resources: CxxTransactionResources,
    fee_config: CxxFeeConfiguration,
) -> Result<FeePair, Box<dyn std::error::Error>> {
    let hm = get_host_module_for_protocol(config_max_protocol, protocol_version)?;
    Ok((hm.compute_transaction_resource_fee)(
        tx_resources,
        fee_config,
    ))
}

pub(crate) fn can_parse_transaction(
    config_max_protocol: u32,
    protocol_version: u32,
    xdr: &CxxBuf,
    depth_limit: u32,
) -> Result<bool, Box<dyn std::error::Error>> {
    let hm = get_host_module_for_protocol(config_max_protocol, protocol_version)?;
    Ok((hm.can_parse_transaction)(xdr, depth_limit))
}

pub(crate) fn compute_rent_fee(
    config_max_protocol: u32,
    protocol_version: u32,
    changed_entries: &Vec<CxxLedgerEntryRentChange>,
    fee_config: CxxRentFeeConfiguration,
    current_ledger_seq: u32,
) -> Result<i64, Box<dyn std::error::Error>> {
    let hm = get_host_module_for_protocol(config_max_protocol, protocol_version)?;
    Ok((hm.compute_rent_fee)(
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
    let hm = get_host_module_for_protocol(config_max_protocol, protocol_version)?;
    Ok((hm.compute_write_fee_per_1kb)(bucket_list_size, fee_config))
}
