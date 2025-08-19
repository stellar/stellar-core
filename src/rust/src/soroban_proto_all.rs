use crate::{
    CxxBuf, CxxFeeConfiguration, CxxLedgerEntryRentChange, CxxLedgerInfo, CxxRentFeeConfiguration,
    CxxRentWriteFeeConfiguration, CxxTransactionResources, FeePair, InvokeHostFunctionOutput,
    SorobanModuleCache, SorobanVersionInfo,
};

#[cfg(feature = "testutils")]
use crate::RustBuf;

// We have multiple copies of soroban linked into stellar-core here. This is
// accomplished using a protocol-agnostic helper module -- soroban_proto_any.rs
// -- mounted multiple times here inside different protocol _adaptor_ modules
// p21, p22, p23, etc. each with its own local binding for the external crate
// soroban_env_host, as well as any protocol-specific bindings that the
// protocol-agnostic code needs to adapt to the differing hosts.
//
// The soroban_proto_any.rs module imports soroban_env_host from its `super` --
// which is the p21, p22, p23, etc. adaptor module here -- which means each
// "instance" of the protocol-agnostic module "sees" a different soroban, even
// though they all use the same name for it. This is a bit of a hack and only
// works when the soroban versions all have a compatible _enough_ interface to
// all be called from "the same" soroban_proto_an.rs (plus or minus the adaptor
// bits here).
//
// Finally there is a bunch of "dispatch" code at the end of this file that
// allows looking up first class functions that call into the correct copy of
// the protocol-agnostic code for a given (runtime-selected) protocol version.
//
// This allows us to pick a protocol version at runtime, and then call the
// correct soroban.

// We also alias the latest soroban as soroban_curr to help reduce churn in code
// that's just "always supposed to use the latest".
pub(crate) use p23 as soroban_curr;

// We also pin some protocol _agnostic_ definitions that are technically
// implemented by a specific version of soroban, but which is protocol-stable
// and can be used across protocols (either by never changing or by being
// only compatible with with, say, a rust Dyn interface like Box<dyn Error>).
pub(crate) mod protocol_agnostic {
    pub(crate) fn make_error(msg: &'static str) -> Box<dyn std::error::Error> {
        super::p23::soroban_proto_any::CoreHostError::General(msg).into()
    }

    // The i128 functions are protocol-agnostic because they're too simple to
    // ever plausibly change. If they ever _do_ change we can switch this (and
    // the callers) to pass a protocol number but it seems unlikely.
    pub(crate) use super::p23::soroban_env_host::xdr::int128_helpers;
}

#[path = "."]
pub(crate) mod p23 {
    pub(crate) extern crate soroban_env_host_p23;
    use crate::{
        bridge::rust_bridge::CxxLedgerEntryRentChange,
        rust_bridge::{
            CxxFeeConfiguration, CxxRentFeeConfiguration, CxxRentWriteFeeConfiguration,
            CxxTransactionResources,
        },
        SorobanModuleCache,
    };
    use soroban_env_host::{
        budget::Budget,
        e2e_invoke::{self, InvokeHostFunctionResult},
        fees::{
            compute_rent_write_fee_per_1kb, FeeConfiguration, LedgerEntryRentChange,
            RentFeeConfiguration, RentWriteFeeConfiguration, TransactionResources,
        },
        vm::wasm_module_memory_cost,
        xdr::{ContractCodeEntry, DiagnosticEvent},
        HostError, LedgerInfo, TraceHook,
    };
    pub(crate) use soroban_env_host_p23 as soroban_env_host;

    pub(crate) mod soroban_proto_any;

    // We do some more local re-exports here of things used in soroban_proto_any.rs that
    // don't exist in older hosts (eg. the p21 & 22 hosts, where we define stubs for
    // these imports).
    pub(crate) use soroban_env_host::{CompilationContext, ErrorHandler, ModuleCache};

    // An adapter for some API breakage between p21 and p22.
    pub(crate) const fn get_version_pre_release(v: &soroban_env_host::Version) -> u32 {
        v.interface.pre_release
    }

    pub(crate) const fn get_version_protocol(v: &soroban_env_host::Version) -> u32 {
        v.interface.protocol
    }

    pub fn invoke_host_function_with_trace_hook_and_module_cache<
        T: AsRef<[u8]>,
        I: ExactSizeIterator<Item = T>,
    >(
        budget: &Budget,
        enable_diagnostics: bool,
        encoded_host_fn: T,
        encoded_resources: T,
        restored_rw_entry_indices: &[u32],
        encoded_source_account: T,
        encoded_auth_entries: I,
        ledger_info: LedgerInfo,
        encoded_ledger_entries: I,
        encoded_ttl_entries: I,
        base_prng_seed: T,
        diagnostic_events: &mut Vec<DiagnosticEvent>,
        trace_hook: Option<TraceHook>,
        module_cache: &SorobanModuleCache,
    ) -> Result<InvokeHostFunctionResult, HostError> {
        e2e_invoke::invoke_host_function(
            budget,
            enable_diagnostics,
            encoded_host_fn,
            encoded_resources,
            restored_rw_entry_indices,
            encoded_source_account,
            encoded_auth_entries,
            ledger_info,
            encoded_ledger_entries,
            encoded_ttl_entries,
            base_prng_seed,
            diagnostic_events,
            trace_hook,
            Some(module_cache.p23_cache.module_cache.clone()),
        )
    }

    pub(crate) fn wasm_module_memory_cost_wrapper(
        budget: &Budget,
        contract_code_entry: &ContractCodeEntry,
    ) -> Result<u64, HostError> {
        wasm_module_memory_cost(budget, contract_code_entry)
    }

    pub(crate) fn compute_rent_write_fee_per_1kb_wrapper(
        bucket_list_size: i64,
        fee_config: CxxRentWriteFeeConfiguration,
    ) -> i64 {
        compute_rent_write_fee_per_1kb(bucket_list_size, &fee_config.into())
    }

    pub(crate) fn convert_transaction_resources(
        value: &CxxTransactionResources,
    ) -> TransactionResources {
        TransactionResources {
            instructions: value.instructions,
            disk_read_entries: value.disk_read_entries,
            write_entries: value.write_entries,
            disk_read_bytes: value.disk_read_bytes,
            write_bytes: value.write_bytes,
            contract_events_size_bytes: value.contract_events_size_bytes,
            transaction_size_bytes: value.transaction_size_bytes,
        }
    }

    impl From<CxxRentWriteFeeConfiguration> for RentWriteFeeConfiguration {
        fn from(value: CxxRentWriteFeeConfiguration) -> Self {
            Self {
                state_target_size_bytes: value.state_target_size_bytes,
                rent_fee_1kb_state_size_low: value.rent_fee_1kb_state_size_low,
                rent_fee_1kb_state_size_high: value.rent_fee_1kb_state_size_high,
                state_size_rent_fee_growth_factor: value.state_size_rent_fee_growth_factor,
            }
        }
    }

    pub(crate) fn convert_rent_fee_configuration(
        value: &CxxRentFeeConfiguration,
    ) -> RentFeeConfiguration {
        RentFeeConfiguration {
            fee_per_rent_1kb: value.fee_per_rent_1kb,
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_write_entry: value.fee_per_write_entry,
            persistent_rent_rate_denominator: value.persistent_rent_rate_denominator,
            temporary_rent_rate_denominator: value.temporary_rent_rate_denominator,
        }
    }

    pub(crate) fn convert_fee_configuration(value: CxxFeeConfiguration) -> FeeConfiguration {
        FeeConfiguration {
            fee_per_instruction_increment: value.fee_per_instruction_increment,
            fee_per_disk_read_entry: value.fee_per_disk_read_entry,
            fee_per_write_entry: value.fee_per_write_entry,
            fee_per_disk_read_1kb: value.fee_per_disk_read_1kb,
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_historical_1kb: value.fee_per_historical_1kb,
            fee_per_contract_event_1kb: value.fee_per_contract_event_1kb,
            fee_per_transaction_size_1kb: value.fee_per_transaction_size_1kb,
        }
    }

    pub(crate) fn convert_ledger_entry_rent_change(
        value: &CxxLedgerEntryRentChange,
    ) -> LedgerEntryRentChange {
        LedgerEntryRentChange {
            is_persistent: value.is_persistent,
            is_code_entry: value.is_code_entry,
            old_size_bytes: value.old_size_bytes,
            new_size_bytes: value.new_size_bytes,
            old_live_until_ledger: value.old_live_until_ledger,
            new_live_until_ledger: value.new_live_until_ledger,
        }
    }
}

#[path = "."]
pub(crate) mod p22 {
    pub(crate) extern crate soroban_env_host_p22;
    pub(crate) use soroban_env_host_p22 as soroban_env_host;
    pub(crate) mod soroban_proto_any;
    use crate::{
        bridge::rust_bridge::CxxLedgerEntryRentChange,
        rust_bridge::{
            CxxFeeConfiguration, CxxRentFeeConfiguration, CxxRentWriteFeeConfiguration,
            CxxTransactionResources,
        },
        SorobanModuleCache,
    };
    use soroban_env_host::{
        budget::{AsBudget, Budget},
        e2e_invoke::{self, InvokeHostFunctionResult},
        fees::{
            compute_write_fee_per_1kb, FeeConfiguration, LedgerEntryRentChange,
            RentFeeConfiguration, TransactionResources, WriteFeeConfiguration,
        },
        xdr::{ContractCodeEntry, DiagnosticEvent, Hash},
        Error, HostError, LedgerInfo, TraceHook, Val,
    };

    // Some stub definitions to handle API additions for the
    // reusable module cache.

    #[allow(dead_code)]
    const INTERNAL_ERROR: Error = Error::from_type_and_code(
        soroban_env_host::xdr::ScErrorType::Context,
        soroban_env_host::xdr::ScErrorCode::InternalError,
    );

    #[allow(dead_code)]
    #[derive(Clone)]
    pub(crate) struct ModuleCache;
    #[allow(dead_code)]
    pub(crate) trait ErrorHandler {
        fn map_err<T, E>(&self, res: Result<T, E>) -> Result<T, HostError>
        where
            Error: From<E>,
            E: core::fmt::Debug;
        fn error(&self, error: Error, msg: &str, args: &[Val]) -> HostError;
    }
    #[allow(dead_code)]
    impl ModuleCache {
        pub(crate) fn new<T>(_handler: T) -> Result<Self, HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn parse_and_cache_module_simple<T>(
            &self,
            _handler: &T,
            _protocol: u32,
            _wasm: &[u8],
        ) -> Result<(), HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn remove_module(&self, _key: &Hash) -> Result<(), HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn clear(&self) -> Result<(), HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn contains_module(&self, _key: &Hash) -> Result<bool, HostError> {
            Err(INTERNAL_ERROR.into())
        }
    }
    #[allow(dead_code)]
    pub(crate) trait CompilationContext: ErrorHandler + AsBudget {}

    // An adapter for some API breakage between p21 and p22.
    pub(crate) const fn get_version_pre_release(v: &soroban_env_host::Version) -> u32 {
        v.interface.pre_release
    }

    pub(crate) const fn get_version_protocol(v: &soroban_env_host::Version) -> u32 {
        v.interface.protocol
    }

    pub fn invoke_host_function_with_trace_hook_and_module_cache<
        T: AsRef<[u8]>,
        I: ExactSizeIterator<Item = T>,
    >(
        budget: &Budget,
        enable_diagnostics: bool,
        encoded_host_fn: T,
        encoded_resources: T,
        _restored_rw_entry_indices: &[u32], // Ignored before p23
        encoded_source_account: T,
        encoded_auth_entries: I,
        ledger_info: LedgerInfo,
        encoded_ledger_entries: I,
        encoded_ttl_entries: I,
        base_prng_seed: T,
        diagnostic_events: &mut Vec<DiagnosticEvent>,
        trace_hook: Option<TraceHook>,
        _module_cache: &SorobanModuleCache,
    ) -> Result<InvokeHostFunctionResult, HostError> {
        e2e_invoke::invoke_host_function_with_trace_hook(
            &budget,
            enable_diagnostics,
            encoded_host_fn,
            encoded_resources,
            encoded_source_account,
            encoded_auth_entries,
            ledger_info,
            encoded_ledger_entries,
            encoded_ttl_entries,
            base_prng_seed,
            diagnostic_events,
            trace_hook,
        )
    }

    pub(crate) fn wasm_module_memory_cost_wrapper(
        _budget: &Budget,
        _contract_code_entry: &ContractCodeEntry,
    ) -> Result<u64, HostError> {
        Err(INTERNAL_ERROR.into())
    }

    pub(crate) fn compute_rent_write_fee_per_1kb_wrapper(
        bucket_list_size: i64,
        fee_config: CxxRentWriteFeeConfiguration,
    ) -> i64 {
        compute_write_fee_per_1kb(bucket_list_size, &fee_config.into())
    }

    pub(crate) fn convert_transaction_resources(
        value: &CxxTransactionResources,
    ) -> TransactionResources {
        TransactionResources {
            instructions: value.instructions,
            read_entries: value.disk_read_entries,
            write_entries: value.write_entries,
            read_bytes: value.disk_read_bytes,
            write_bytes: value.write_bytes,
            contract_events_size_bytes: value.contract_events_size_bytes,
            transaction_size_bytes: value.transaction_size_bytes,
        }
    }

    impl From<CxxRentWriteFeeConfiguration> for WriteFeeConfiguration {
        fn from(value: CxxRentWriteFeeConfiguration) -> Self {
            Self {
                bucket_list_target_size_bytes: value.state_target_size_bytes,
                write_fee_1kb_bucket_list_low: value.rent_fee_1kb_state_size_low,
                write_fee_1kb_bucket_list_high: value.rent_fee_1kb_state_size_high,
                bucket_list_write_fee_growth_factor: value.state_size_rent_fee_growth_factor,
            }
        }
    }
    pub(crate) fn convert_rent_fee_configuration(
        value: &CxxRentFeeConfiguration,
    ) -> RentFeeConfiguration {
        RentFeeConfiguration {
            fee_per_write_1kb: value.fee_per_rent_1kb,
            fee_per_write_entry: value.fee_per_write_entry,
            persistent_rent_rate_denominator: value.persistent_rent_rate_denominator,
            temporary_rent_rate_denominator: value.temporary_rent_rate_denominator,
        }
    }

    pub(crate) fn convert_fee_configuration(value: CxxFeeConfiguration) -> FeeConfiguration {
        FeeConfiguration {
            fee_per_instruction_increment: value.fee_per_instruction_increment,
            fee_per_read_entry: value.fee_per_disk_read_entry,
            fee_per_write_entry: value.fee_per_write_entry,
            fee_per_read_1kb: value.fee_per_disk_read_1kb,
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_historical_1kb: value.fee_per_historical_1kb,
            fee_per_contract_event_1kb: value.fee_per_contract_event_1kb,
            fee_per_transaction_size_1kb: value.fee_per_transaction_size_1kb,
        }
    }

    pub(crate) fn convert_ledger_entry_rent_change(
        value: &CxxLedgerEntryRentChange,
    ) -> LedgerEntryRentChange {
        LedgerEntryRentChange {
            is_persistent: value.is_persistent,
            old_size_bytes: value.old_size_bytes,
            new_size_bytes: value.new_size_bytes,
            old_live_until_ledger: value.old_live_until_ledger,
            new_live_until_ledger: value.new_live_until_ledger,
        }
    }
}

#[path = "."]
pub(crate) mod p21 {
    pub(crate) extern crate soroban_env_host_p21;
    pub(crate) use soroban_env_host_p21 as soroban_env_host;
    pub(crate) mod soroban_proto_any;
    use crate::{
        bridge::rust_bridge::CxxLedgerEntryRentChange,
        rust_bridge::{
            CxxFeeConfiguration, CxxRentFeeConfiguration, CxxRentWriteFeeConfiguration,
            CxxTransactionResources,
        },
        SorobanModuleCache,
    };
    use soroban_env_host::{
        budget::{AsBudget, Budget},
        e2e_invoke::{self, InvokeHostFunctionResult},
        fees::{
            compute_write_fee_per_1kb, FeeConfiguration, LedgerEntryRentChange,
            RentFeeConfiguration, TransactionResources, WriteFeeConfiguration,
        },
        xdr::{ContractCodeEntry, DiagnosticEvent, Hash},
        Error, HostError, LedgerInfo, TraceHook, Val,
    };

    // Some stub definitions to handle API additions for the
    // reusable module cache.

    #[allow(dead_code)]
    const INTERNAL_ERROR: Error = Error::from_type_and_code(
        soroban_env_host::xdr::ScErrorType::Context,
        soroban_env_host::xdr::ScErrorCode::InternalError,
    );

    #[allow(dead_code)]
    #[derive(Clone)]
    pub(crate) struct ModuleCache;
    #[allow(dead_code)]
    pub(crate) trait ErrorHandler {
        fn map_err<T, E>(&self, res: Result<T, E>) -> Result<T, HostError>
        where
            Error: From<E>,
            E: core::fmt::Debug;
        fn error(&self, error: Error, msg: &str, args: &[Val]) -> HostError;
    }
    #[allow(dead_code)]
    impl ModuleCache {
        pub(crate) fn new<T>(_handler: T) -> Result<Self, HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn parse_and_cache_module_simple<T>(
            &self,
            _handler: &T,
            _protocol: u32,
            _wasm: &[u8],
        ) -> Result<(), HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn remove_module(&self, _key: &Hash) -> Result<(), HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn clear(&self) -> Result<(), HostError> {
            Err(INTERNAL_ERROR.into())
        }
        pub(crate) fn contains_module(&self, _key: &Hash) -> Result<bool, HostError> {
            Err(INTERNAL_ERROR.into())
        }
    }
    #[allow(dead_code)]
    pub(crate) trait CompilationContext: ErrorHandler + AsBudget {}

    // An adapter for some API breakage between p21 and p22.
    pub(crate) const fn get_version_pre_release(v: &soroban_env_host::Version) -> u32 {
        soroban_env_host::meta::get_pre_release_version(v.interface)
    }

    pub(crate) const fn get_version_protocol(v: &soroban_env_host::Version) -> u32 {
        soroban_env_host::meta::get_ledger_protocol_version(v.interface)
    }

    pub fn invoke_host_function_with_trace_hook_and_module_cache<
        T: AsRef<[u8]>,
        I: ExactSizeIterator<Item = T>,
    >(
        budget: &Budget,
        enable_diagnostics: bool,
        encoded_host_fn: T,
        encoded_resources: T,
        _restored_rw_entry_indices: &[u32], // Ignored before p23
        encoded_source_account: T,
        encoded_auth_entries: I,
        ledger_info: LedgerInfo,
        encoded_ledger_entries: I,
        encoded_ttl_entries: I,
        base_prng_seed: T,
        diagnostic_events: &mut Vec<DiagnosticEvent>,
        trace_hook: Option<TraceHook>,
        _module_cache: &SorobanModuleCache,
    ) -> Result<InvokeHostFunctionResult, HostError> {
        e2e_invoke::invoke_host_function_with_trace_hook(
            &budget,
            enable_diagnostics,
            encoded_host_fn,
            encoded_resources,
            encoded_source_account,
            encoded_auth_entries,
            ledger_info,
            encoded_ledger_entries,
            encoded_ttl_entries,
            base_prng_seed,
            diagnostic_events,
            trace_hook,
        )
    }

    pub(crate) fn wasm_module_memory_cost_wrapper(
        _budget: &Budget,
        _contract_code_entry: &ContractCodeEntry,
    ) -> Result<u64, HostError> {
        Err(INTERNAL_ERROR.into())
    }

    pub(crate) fn compute_rent_write_fee_per_1kb_wrapper(
        bucket_list_size: i64,
        fee_config: CxxRentWriteFeeConfiguration,
    ) -> i64 {
        compute_write_fee_per_1kb(bucket_list_size, &fee_config.into())
    }

    pub(crate) fn convert_transaction_resources(
        value: &CxxTransactionResources,
    ) -> TransactionResources {
        TransactionResources {
            instructions: value.instructions,
            read_entries: value.disk_read_entries,
            write_entries: value.write_entries,
            read_bytes: value.disk_read_bytes,
            write_bytes: value.write_bytes,
            contract_events_size_bytes: value.contract_events_size_bytes,
            transaction_size_bytes: value.transaction_size_bytes,
        }
    }

    impl From<CxxRentWriteFeeConfiguration> for WriteFeeConfiguration {
        fn from(value: CxxRentWriteFeeConfiguration) -> Self {
            Self {
                bucket_list_target_size_bytes: value.state_target_size_bytes,
                write_fee_1kb_bucket_list_low: value.rent_fee_1kb_state_size_low,
                write_fee_1kb_bucket_list_high: value.rent_fee_1kb_state_size_high,
                bucket_list_write_fee_growth_factor: value.state_size_rent_fee_growth_factor,
            }
        }
    }

    pub(crate) fn convert_rent_fee_configuration(
        value: &CxxRentFeeConfiguration,
    ) -> RentFeeConfiguration {
        RentFeeConfiguration {
            fee_per_write_1kb: value.fee_per_rent_1kb,
            fee_per_write_entry: value.fee_per_write_entry,
            persistent_rent_rate_denominator: value.persistent_rent_rate_denominator,
            temporary_rent_rate_denominator: value.temporary_rent_rate_denominator,
        }
    }

    pub(crate) fn convert_fee_configuration(value: CxxFeeConfiguration) -> FeeConfiguration {
        FeeConfiguration {
            fee_per_instruction_increment: value.fee_per_instruction_increment,
            fee_per_read_entry: value.fee_per_disk_read_entry,
            fee_per_write_entry: value.fee_per_write_entry,
            fee_per_read_1kb: value.fee_per_disk_read_1kb,
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_historical_1kb: value.fee_per_historical_1kb,
            fee_per_contract_event_1kb: value.fee_per_contract_event_1kb,
            fee_per_transaction_size_1kb: value.fee_per_transaction_size_1kb,
        }
    }

    pub(crate) fn convert_ledger_entry_rent_change(
        value: &CxxLedgerEntryRentChange,
    ) -> LedgerEntryRentChange {
        LedgerEntryRentChange {
            is_persistent: value.is_persistent,
            old_size_bytes: value.old_size_bytes,
            new_size_bytes: value.new_size_bytes,
            old_live_until_ledger: value.old_live_until_ledger,
            new_live_until_ledger: value.new_live_until_ledger,
        }
    }
}

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

pub(crate) fn get_soroban_version_info(core_max_proto: u32) -> Vec<SorobanVersionInfo> {
    // This is just a check to ensure the `soroban_curr` alias has been updated
    // to point to the current latest soroban version.
    let curr_max_proto = soroban_curr::soroban_proto_any::get_max_proto();
    if curr_max_proto != core_max_proto {
        panic!(
            "soroban_curr::get_max_proto() {} is not equal to C++ stellar-core max protocol {}",
            curr_max_proto, core_max_proto
        );
    }

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

// Rust does not support first-class modules. This means we cannot put multiple
// modules into an array and iterate over it switching between them by protocol
// number. Which is what we want to do! But as a workaround, we can copy
// everything we want _from_ each module into a struct, and work with the struct
// as a first-class value. This is what we do here.
pub(crate) struct HostModule {
    // Technically the `get_version_info` function returns the max_proto as
    // well, but we want to compute it separately so it can be baked into a
    // compile-time constant to minimize the cost of the protocol-based
    // dispatch. The struct returned from `get_version_info` contains a bunch of
    // dynamic strings, which is necessary due to cxx limitations.
    pub(crate) max_proto: u32,
    pub(crate) get_soroban_version_info: fn(u32) -> SorobanVersionInfo,
    pub(crate) invoke_host_function:
        fn(
            enable_diagnostics: bool,
            instruction_limit: u32,
            hf_buf: &CxxBuf,
            resources_buf: &CxxBuf,
            restored_rw_entry_indices: &Vec<u32>,
            source_account_buf: &CxxBuf,
            auth_entries: &Vec<CxxBuf>,
            ledger_info: &CxxLedgerInfo,
            ledger_entries: &Vec<CxxBuf>,
            ttl_entries: &Vec<CxxBuf>,
            base_prng_seed: &CxxBuf,
            rent_fee_configuration: &CxxRentFeeConfiguration,
            module_cache: &SorobanModuleCache,
        ) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>>,
    pub(crate) compute_transaction_resource_fee:
        fn(tx_resources: CxxTransactionResources, fee_config: CxxFeeConfiguration) -> FeePair,
    pub(crate) compute_rent_fee: fn(
        changed_entries: &Vec<CxxLedgerEntryRentChange>,
        fee_config: CxxRentFeeConfiguration,
        current_ledger_seq: u32,
    ) -> i64,
    pub(crate) compute_rent_write_fee_per_1kb:
        fn(bucket_list_size: i64, fee_config: CxxRentWriteFeeConfiguration) -> i64,
    pub(crate) contract_code_memory_size_for_rent: fn(
        contract_code_entry: &CxxBuf,
        cpu_cost_params: &CxxBuf,
        mem_cost_params: &CxxBuf,
    )
        -> Result<u32, Box<dyn std::error::Error>>,
    pub(crate) can_parse_transaction: fn(&CxxBuf, depth_limit: u32) -> bool,
    #[cfg(feature = "testutils")]
    pub(crate) rustbuf_containing_scval_to_string: fn(&RustBuf) -> String,
    #[cfg(feature = "testutils")]
    pub(crate) rustbuf_containing_diagnostic_event_to_string: fn(&RustBuf) -> String,
}

macro_rules! proto_versioned_functions_for_module {
    ($module:ident) => {
        HostModule {
            max_proto: $module::soroban_proto_any::get_max_proto(),
            get_soroban_version_info: $module::soroban_proto_any::get_soroban_version_info,
            invoke_host_function: $module::soroban_proto_any::invoke_host_function,
            compute_transaction_resource_fee:
                $module::soroban_proto_any::compute_transaction_resource_fee,
            compute_rent_fee: $module::soroban_proto_any::compute_rent_fee,
            compute_rent_write_fee_per_1kb:
                $module::soroban_proto_any::compute_rent_write_fee_per_1kb,
            contract_code_memory_size_for_rent:
                $module::soroban_proto_any::contract_code_memory_size_for_rent,
            can_parse_transaction: $module::soroban_proto_any::can_parse_transaction,
            #[cfg(feature = "testutils")]
            rustbuf_containing_scval_to_string:
                $module::soroban_proto_any::rustbuf_containing_scval_to_string,
            #[cfg(feature = "testutils")]
            rustbuf_containing_diagnostic_event_to_string:
                $module::soroban_proto_any::rustbuf_containing_diagnostic_event_to_string,
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

pub(crate) fn get_host_module_for_protocol(
    config_max_protocol: u32,
    ledger_protocol_version: u32,
) -> Result<&'static HostModule, Box<dyn std::error::Error>> {
    if ledger_protocol_version > config_max_protocol {
        return Err(protocol_agnostic::make_error(
            "protocol exceeds configured max",
        ));
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
    Err(protocol_agnostic::make_error("unsupported protocol"))
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

    // Ledger protocol has to be less than config max.
    assert!(get_host_module_for_protocol(20, 21).is_err());
}
