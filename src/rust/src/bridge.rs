// The cxx::bridge attribute says that everything in mod rust_bridge is
// interpreted by cxx.rs.
#[cxx::bridge]
pub(crate) mod rust_bridge {
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

    #[derive(Debug, PartialEq, Eq)]
    struct CxxI128 {
        hi: i64,
        lo: u64,
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
            module_cache: &SorobanModuleCache,
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

        fn i128_add(lhs: &CxxI128, rhs: &CxxI128) -> Result<CxxI128>;

        fn i128_sub(lhs: &CxxI128, rhs: &CxxI128) -> Result<CxxI128>;

        fn i128_add_will_overflow(lhs: &CxxI128, rhs: &CxxI128) -> Result<bool>;

        fn i128_sub_will_underflow(lhs: &CxxI128, rhs: &CxxI128) -> Result<bool>;

        fn i128_from_i64(val: i64) -> Result<CxxI128>;

        type SorobanModuleCache;

        fn new_module_cache() -> Result<Box<SorobanModuleCache>>;
        fn compile(
            self: &mut SorobanModuleCache,
            ledger_protocol: u32,
            source: &[u8],
        ) -> Result<()>;
        fn shallow_clone(self: &SorobanModuleCache) -> Result<Box<SorobanModuleCache>>;
        fn evict_contract_code(self: &mut SorobanModuleCache, key: &[u8]) -> Result<()>;
        fn clear(self: &mut SorobanModuleCache) -> Result<()>;
        fn contains_module(self: &SorobanModuleCache, protocol: u32, key: &[u8]) -> Result<bool>;
        fn get_mem_bytes_consumed(self: &SorobanModuleCache) -> Result<u64>;
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

// Because this file is processed with cxx.rs, we need to "use" the
// definitions of all the symbols we declare above, just to make the
// resulting code compile -- they all get referenced in the generated
// code so they have to be in scope here.
use crate::b64::*;
use crate::common::*;
use crate::i128::*;
use crate::log::*;
use crate::soroban_invoke::*;
use crate::soroban_module_cache::*;
use crate::soroban_proto_all::*;
use crate::soroban_test_wasm::*;
