// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBuf, CxxFeeConfiguration, CxxLedgerEntryRentChange, CxxLedgerInfo,
        CxxRentFeeConfiguration, CxxTransactionResources, CxxWriteFeeConfiguration, FeePair,
        InvokeHostFunctionOutput, RustBuf, SorobanVersionInfo, XDRFileHash,
    },
};
use log::{debug, trace, warn};
use std::{fmt::Display, io::Cursor, panic, rc::Rc, time::Instant};

// This module (contract) is bound to _two separate locations_ in the module
// tree: crate::lo::contract and crate::hi::contract, each of which has a (lo or
// hi) version-specific definition of stellar_env_host. We therefore
// import it from our _parent_ module rather than from the crate root.
pub(crate) use super::soroban_env_host::{
    budget::Budget,
    e2e_invoke::{self, extract_rent_changes, LedgerEntryChange},
    fees::{
        compute_rent_fee as host_compute_rent_fee,
        compute_transaction_resource_fee as host_compute_transaction_resource_fee,
        compute_write_fee_per_1kb as host_compute_write_fee_per_1kb, FeeConfiguration,
        LedgerEntryRentChange, RentFeeConfiguration, TransactionResources, WriteFeeConfiguration,
    },
    xdr::{
        self, ContractCostParams, ContractEvent, ContractEventBody, ContractEventType,
        ContractEventV0, DiagnosticEvent, ExtensionPoint, LedgerEntry, LedgerEntryData,
        LedgerEntryExt, Limits, ReadXdr, ScError, ScErrorCode, ScErrorType, ScSymbol, ScVal,
        TransactionEnvelope, TtlEntry, WriteXdr, XDR_FILES_SHA256,
    },
    HostError, LedgerInfo, VERSION,
};
use std::error::Error;

impl TryFrom<&CxxLedgerInfo> for LedgerInfo {
    type Error = Box<dyn Error>;
    fn try_from(c: &CxxLedgerInfo) -> Result<Self, Self::Error> {
        Ok(Self {
            protocol_version: c.protocol_version,
            sequence_number: c.sequence_number,
            timestamp: c.timestamp,
            network_id: c
                .network_id
                .clone()
                .try_into()
                .map_err(|_| Box::new(CoreHostError::General("network ID has wrong size")))?,
            base_reserve: c.base_reserve,
            min_temp_entry_ttl: c.min_temp_entry_ttl,
            min_persistent_entry_ttl: c.min_persistent_entry_ttl,
            max_entry_ttl: c.max_entry_ttl,
        })
    }
}

impl From<CxxTransactionResources> for TransactionResources {
    fn from(value: CxxTransactionResources) -> Self {
        Self {
            instructions: value.instructions,
            read_entries: value.read_entries,
            write_entries: value.write_entries,
            read_bytes: value.read_bytes,
            write_bytes: value.write_bytes,
            contract_events_size_bytes: value.contract_events_size_bytes,
            transaction_size_bytes: value.transaction_size_bytes,
        }
    }
}

impl From<CxxFeeConfiguration> for FeeConfiguration {
    fn from(value: CxxFeeConfiguration) -> Self {
        Self {
            fee_per_instruction_increment: value.fee_per_instruction_increment,
            fee_per_read_entry: value.fee_per_read_entry,
            fee_per_write_entry: value.fee_per_write_entry,
            fee_per_read_1kb: value.fee_per_read_1kb,
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_historical_1kb: value.fee_per_historical_1kb,
            fee_per_contract_event_1kb: value.fee_per_contract_event_1kb,
            fee_per_transaction_size_1kb: value.fee_per_transaction_size_1kb,
        }
    }
}

impl From<&CxxLedgerEntryRentChange> for LedgerEntryRentChange {
    fn from(value: &CxxLedgerEntryRentChange) -> Self {
        Self {
            is_persistent: value.is_persistent,
            old_size_bytes: value.old_size_bytes,
            new_size_bytes: value.new_size_bytes,
            old_live_until_ledger: value.old_live_until_ledger,
            new_live_until_ledger: value.new_live_until_ledger,
        }
    }
}

impl From<&CxxRentFeeConfiguration> for RentFeeConfiguration {
    fn from(value: &CxxRentFeeConfiguration) -> Self {
        Self {
            fee_per_write_1kb: value.fee_per_write_1kb,
            fee_per_write_entry: value.fee_per_write_entry,
            persistent_rent_rate_denominator: value.persistent_rent_rate_denominator,
            temporary_rent_rate_denominator: value.temporary_rent_rate_denominator,
        }
    }
}

impl From<CxxWriteFeeConfiguration> for WriteFeeConfiguration {
    fn from(value: CxxWriteFeeConfiguration) -> Self {
        Self {
            bucket_list_target_size_bytes: value.bucket_list_target_size_bytes,
            write_fee_1kb_bucket_list_low: value.write_fee_1kb_bucket_list_low,
            write_fee_1kb_bucket_list_high: value.write_fee_1kb_bucket_list_high,
            bucket_list_write_fee_growth_factor: value.bucket_list_write_fee_growth_factor,
        }
    }
}

// FIXME: plumb this through from the limit xdrpp uses.
// Currently they are just two same-valued constants.
const MARSHALLING_STACK_LIMIT: u32 = 1000;

#[allow(dead_code)]
#[derive(Debug)]
pub(crate) enum CoreHostError {
    Host(HostError),
    General(&'static str),
}

impl Display for CoreHostError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl From<HostError> for CoreHostError {
    fn from(h: HostError) -> Self {
        CoreHostError::Host(h)
    }
}

impl From<xdr::Error> for CoreHostError {
    fn from(_: xdr::Error) -> Self {
        CoreHostError::Host((ScErrorType::Value, ScErrorCode::InvalidInput).into())
    }
}

impl std::error::Error for CoreHostError {}

fn non_metered_xdr_from_cxx_buf<T: ReadXdr>(buf: &CxxBuf) -> Result<T, HostError> {
    Ok(T::read_xdr(&mut xdr::Limited::new(
        Cursor::new(buf.data.as_slice()),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: buf.data.len(),
        },
    ))
    // We only expect this to be called for safe, internal conversions, so this
    // should never happen.
    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?)
}

fn non_metered_xdr_to_vec<T: WriteXdr>(t: &T) -> Result<Vec<u8>, HostError> {
    let mut vec: Vec<u8> = Vec::new();
    t.write_xdr(&mut xdr::Limited::new(
        Cursor::new(&mut vec),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: 5 * 1024 * 1024, /* 5MB */
        },
    ))
    .map_err(|_| (ScErrorType::Value, ScErrorCode::InvalidInput))?;
    Ok(vec)
}

fn non_metered_xdr_to_rust_buf<T: WriteXdr>(t: &T) -> Result<RustBuf, HostError> {
    Ok(RustBuf {
        data: non_metered_xdr_to_vec(t)?,
    })
}

// This is just a helper for modifying some data that is encoded in a CxxBuf. It
// decodes the data, modifies it, and then re-encodes it back into the CxxBuf.
// It's intended for use when modifying the cost parameters of the CxxLedgerInfo
// when invoking a contract twice with different protocols.
#[allow(dead_code)]
#[cfg(feature = "testutils")]
pub(crate) fn inplace_modify_cxxbuf_encoded_type<T: ReadXdr + WriteXdr>(
    buf: &mut CxxBuf,
    modify: impl FnOnce(&mut T) -> Result<(), Box<dyn Error>>,
) -> Result<(), Box<dyn Error>> {
    let mut tmp = non_metered_xdr_from_cxx_buf::<T>(buf)?;
    modify(&mut tmp)?;
    let vec = non_metered_xdr_to_vec::<T>(&tmp)?;
    buf.replace_data_with(vec.as_slice())
}

/// Returns a vec of [`XDRFileHash`] structs each representing one .x file
/// that served as input to xdrgen, which created the XDR definitions used in
/// the Rust crates visible here. This allows the C++ side of the bridge
/// to confirm that the same definitions are compiled into the C++ code.
pub fn get_xdr_hashes() -> Vec<XDRFileHash> {
    XDR_FILES_SHA256
        .iter()
        .map(|(file, hash)| XDRFileHash {
            file: (*file).into(),
            hash: (*hash).into(),
        })
        .collect()
}

pub const fn get_max_proto() -> u32 {
    super::get_version_protocol(&VERSION)
}

pub fn get_soroban_version_info(core_max_proto: u32) -> SorobanVersionInfo {
    let env_max_proto = get_max_proto();
    let xdr_base_git_rev = match VERSION.xdr.xdr {
        "curr" => VERSION.xdr.xdr_curr.to_string(),
        "next" | "curr,next" => {
            if !cfg!(feature = "next") {
                warn!(
                    "soroban version {} XDR module built with 'next' feature,
                       but core built without 'vnext' feature",
                    VERSION.pkg
                );
            }
            if core_max_proto != env_max_proto {
                warn!(
                    "soroban version {} XDR module for env version {} built with 'next' feature, \
                       even though this is not the newest core protocol ({})",
                    VERSION.pkg, env_max_proto, core_max_proto
                );
                warn!(
                    "this can happen if multiple soroban crates depend on the \
                       same XDR crate which then gets feature-unified"
                )
            }
            VERSION.xdr.xdr_next.to_string()
        }
        other => format!("unknown XDR module configuration: '{other}'"),
    };

    SorobanVersionInfo {
        env_max_proto,
        env_pkg_ver: VERSION.pkg.to_string(),
        env_git_rev: VERSION.rev.to_string(),
        env_pre_release_ver: super::get_version_pre_release(&VERSION),
        xdr_pkg_ver: VERSION.xdr.pkg.to_string(),
        xdr_git_rev: VERSION.xdr.rev.to_string(),
        xdr_base_git_rev,
        xdr_file_hashes: get_xdr_hashes(),
    }
}

fn log_diagnostic_events(events: &Vec<DiagnosticEvent>) {
    for e in events {
        debug!("Diagnostic event: {:?}", e);
    }
}

fn encode_diagnostic_events(events: &Vec<DiagnosticEvent>) -> Vec<RustBuf> {
    events
        .iter()
        .filter_map(|e| {
            if let Ok(encoded) = non_metered_xdr_to_rust_buf(e) {
                Some(encoded)
            } else {
                None
            }
        })
        .collect()
}

fn extract_ledger_effects(
    entry_changes: Vec<LedgerEntryChange>,
) -> Result<Vec<RustBuf>, HostError> {
    let mut modified_entries = vec![];

    for change in entry_changes {
        // Extract ContractCode and ContractData entry changes first
        if !change.read_only {
            if let Some(encoded_new_value) = change.encoded_new_value {
                modified_entries.push(encoded_new_value.into());
            }
        }

        // Check for TtlEntry changes
        if let Some(ttl_change) = change.ttl_change {
            if ttl_change.new_live_until_ledger > ttl_change.old_live_until_ledger {
                // entry_changes only encode LedgerEntry changes for ContractCode and ContractData
                // entries. Changes to TtlEntry are recorded in ttl_change, but does
                // not contain an encoded TtlEntry. We must build that here.
                let hash_bytes: [u8; 32] = ttl_change
                    .key_hash
                    .try_into()
                    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?;

                let le = LedgerEntry {
                    last_modified_ledger_seq: 0,
                    data: LedgerEntryData::Ttl(TtlEntry {
                        key_hash: hash_bytes.into(),
                        live_until_ledger_seq: ttl_change.new_live_until_ledger,
                    }),
                    ext: LedgerEntryExt::V0,
                };

                let encoded = non_metered_xdr_to_rust_buf(&le)
                    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?;
                modified_entries.push(encoded);
            }
        }
    }

    Ok(modified_entries)
}

/// Deserializes an [`xdr::HostFunction`] host function XDR object an
/// [`xdr::Footprint`] and a sequence of [`xdr::LedgerEntry`] entries containing all
/// the data the invocation intends to read. Then calls the specified host function
/// and returns the [`InvokeHostFunctionOutput`] that contains the host function
/// result, events and modified ledger entries. Ledger entries not returned have
/// been deleted.
pub(crate) fn invoke_host_function(
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
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    let res = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        invoke_host_function_or_maybe_panic(
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
    }));
    match res {
        Err(_) => Err(CoreHostError::General("contract host panicked").into()),
        Ok(r) => r,
    }
}

fn make_trace_hook_fn<'a>() -> super::soroban_env_host::TraceHook {
    let prev_state = std::cell::RefCell::new(String::new());
    Rc::new(move |host, traceevent| {
        if traceevent.is_begin() || traceevent.is_end() {
            prev_state.replace(String::new());
        }
        match super::soroban_env_host::TraceRecord::new(host, traceevent) {
            Ok(tr) => {
                let state_str = format!("{}", tr.state);
                if prev_state.borrow().is_empty() {
                    trace!(target: TX, "{}: {}", tr.event, state_str);
                } else {
                    let diff = crate::log::diff_line(&prev_state.borrow(), &state_str);
                    trace!(target: TX, "{}: {}", tr.event, diff);
                }
                prev_state.replace(state_str);
            }
            Err(e) => trace!(target: TX, "{}", e),
        }
        Ok(())
    })
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
fn decode_contract_cost_params(buf: &CxxBuf) -> Result<ContractCostParams, Box<dyn Error>> {
    Ok(non_metered_xdr_from_cxx_buf::<ContractCostParams>(buf)?)
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
fn encode_contract_cost_params(params: &ContractCostParams) -> Result<RustBuf, Box<dyn Error>> {
    Ok(non_metered_xdr_to_rust_buf(params)?)
}

fn invoke_host_function_or_maybe_panic(
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
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    #[cfg(feature = "tracy")]
    let client = tracy_client::Client::start();
    let _span0 = tracy_span!("invoke_host_function_or_maybe_panic");

    let protocol_version = ledger_info.protocol_version;

    let budget = Budget::try_from_configs(
        instruction_limit as u64,
        ledger_info.memory_limit as u64,
        // These are the only non-metered XDR conversions that we perform. They
        // have a small constant cost that is independent of the user-provided
        // data.
        non_metered_xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.cpu_cost_params)?,
        non_metered_xdr_from_cxx_buf::<ContractCostParams>(&ledger_info.mem_cost_params)?,
    )?;
    let mut diagnostic_events = vec![];
    let ledger_seq_num = ledger_info.sequence_number;
    let trace_hook: Option<super::soroban_env_host::TraceHook> =
        if crate::log::is_tx_tracing_enabled() {
            Some(make_trace_hook_fn())
        } else {
            None
        };
    let (res, time_nsecs) = {
        let _span1 = tracy_span!("e2e_invoke::invoke_function");
        let start_time = Instant::now();
        let res = e2e_invoke::invoke_host_function_with_trace_hook(
            &budget,
            enable_diagnostics,
            hf_buf,
            resources_buf,
            source_account_buf,
            auth_entries.iter(),
            ledger_info.try_into()?,
            ledger_entries.iter(),
            ttl_entries.iter(),
            base_prng_seed,
            &mut diagnostic_events,
            trace_hook,
        );
        let stop_time = Instant::now();
        let time_nsecs = stop_time.duration_since(start_time).as_nanos() as u64;
        (res, time_nsecs)
    };

    // Unconditionally log diagnostic events (there won't be any if diagnostics
    // is disabled).
    log_diagnostic_events(&diagnostic_events);

    let cpu_insns = budget.get_cpu_insns_consumed()?;
    let mem_bytes = budget.get_mem_bytes_consumed()?;
    let cpu_insns_excluding_vm_instantiation = cpu_insns.saturating_sub(
        budget
            .get_tracker(xdr::ContractCostType::VmInstantiation)?
            .cpu,
    );
    let time_nsecs_excluding_vm_instantiation =
        time_nsecs.saturating_sub(budget.get_time(xdr::ContractCostType::VmInstantiation)?);
    #[cfg(feature = "tracy")]
    {
        client.plot(
            tracy_client::plot_name!("soroban budget cpu"),
            cpu_insns as f64,
        );
        client.plot(
            tracy_client::plot_name!("soroban budget mem"),
            mem_bytes as f64,
        );
    }
    let err = match res {
        Ok(res) => match res.encoded_invoke_result {
            Ok(result_value) => {
                let rent_changes = extract_rent_changes(&res.ledger_changes);
                let rent_fee = host_compute_rent_fee(
                    &rent_changes,
                    &rent_fee_configuration.into(),
                    ledger_seq_num,
                );
                let modified_ledger_entries = extract_ledger_effects(res.ledger_changes)?;
                return Ok(InvokeHostFunctionOutput {
                    success: true,
                    is_internal_error: false,
                    diagnostic_events: encode_diagnostic_events(&diagnostic_events),
                    cpu_insns,
                    mem_bytes,
                    time_nsecs,
                    cpu_insns_excluding_vm_instantiation,
                    time_nsecs_excluding_vm_instantiation,

                    result_value: result_value.into(),
                    modified_ledger_entries,
                    contract_events: res
                        .encoded_contract_events
                        .into_iter()
                        .map(RustBuf::from)
                        .collect(),
                    rent_fee,
                });
            }
            Err(e) => e,
        },
        Err(e) => e,
    };
    if enable_diagnostics {
        diagnostic_events.push(DiagnosticEvent {
            in_successful_contract_call: false,
            event: ContractEvent {
                ext: ExtensionPoint::V0,
                contract_id: None,
                type_: ContractEventType::Diagnostic,
                body: ContractEventBody::V0(ContractEventV0 {
                    topics: vec![
                        ScVal::Symbol(ScSymbol("host_fn_failed".try_into().unwrap_or_default())),
                        ScVal::Error(
                            err.error
                                .try_into()
                                .unwrap_or(ScError::Context(ScErrorCode::InternalError)),
                        ),
                    ]
                    .try_into()
                    .unwrap_or_default(),
                    data: ScVal::Void,
                }),
            },
        })
    }
    let is_internal_error = if protocol_version < 22 {
        err.error.is_code(ScErrorCode::InternalError)
    } else {
        err.error.is_code(ScErrorCode::InternalError) && !err.error.is_type(ScErrorType::Contract)
    };

    debug!(target: TX, "invocation failed: {}", err);
    return Ok(InvokeHostFunctionOutput {
        success: false,
        is_internal_error,
        diagnostic_events: encode_diagnostic_events(&diagnostic_events),
        cpu_insns,
        mem_bytes,
        time_nsecs,
        cpu_insns_excluding_vm_instantiation,
        time_nsecs_excluding_vm_instantiation,

        result_value: vec![].into(),
        modified_ledger_entries: vec![],
        contract_events: vec![],
        rent_fee: 0,
    });
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
pub(crate) fn rustbuf_containing_scval_to_string(buf: &RustBuf) -> String {
    if let Ok(val) = ScVal::read_xdr(&mut xdr::Limited::new(
        Cursor::new(buf.data.as_slice()),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: buf.data.len(),
        },
    )) {
        format!("{:?}", val)
    } else {
        "<bad ScVal>".to_string()
    }
}

#[allow(dead_code)]
#[cfg(feature = "testutils")]
pub(crate) fn rustbuf_containing_diagnostic_event_to_string(buf: &RustBuf) -> String {
    if let Ok(val) = DiagnosticEvent::read_xdr(&mut xdr::Limited::new(
        Cursor::new(buf.data.as_slice()),
        Limits {
            depth: MARSHALLING_STACK_LIMIT,
            len: buf.data.len(),
        },
    )) {
        format!("{:?}", val)
    } else {
        "<bad DiagnosticEvent>".to_string()
    }
}

pub(crate) fn compute_transaction_resource_fee(
    tx_resources: CxxTransactionResources,
    fee_config: CxxFeeConfiguration,
) -> FeePair {
    let (non_refundable_fee, refundable_fee) =
        host_compute_transaction_resource_fee(&tx_resources.into(), &fee_config.into());
    FeePair {
        non_refundable_fee,
        refundable_fee,
    }
}

pub(crate) fn compute_rent_fee(
    changed_entries: &Vec<CxxLedgerEntryRentChange>,
    fee_config: CxxRentFeeConfiguration,
    current_ledger_seq: u32,
) -> i64 {
    let changed_entries: Vec<_> = changed_entries.iter().map(|e| e.into()).collect();
    host_compute_rent_fee(
        &changed_entries,
        &((&fee_config).into()),
        current_ledger_seq,
    )
}

pub(crate) fn compute_write_fee_per_1kb(
    bucket_list_size: i64,
    fee_config: CxxWriteFeeConfiguration,
) -> i64 {
    host_compute_write_fee_per_1kb(bucket_list_size, &fee_config.into())
}

pub(crate) fn can_parse_transaction(xdr: &CxxBuf, depth_limit: u32) -> bool {
    let res = TransactionEnvelope::read_xdr(&mut xdr::Limited::new(
        Cursor::new(xdr.data.as_slice()),
        Limits {
            depth: depth_limit,
            len: xdr.data.len(),
        },
    ));
    res.is_ok()
}
