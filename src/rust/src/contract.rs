// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::{
    log::partition::TX,
    rust_bridge::{
        CxxBuf, CxxFeeConfiguration, CxxLedgerEntryRentChange, CxxLedgerInfo,
        CxxRentFeeConfiguration, CxxTransactionResources, CxxWriteFeeConfiguration, FeePair,
        InvokeHostFunctionOutput, RustBuf, XDRFileHash,
    },
};
use log::debug;
use std::{fmt::Display, io::Cursor, panic, time::Instant};

// This module (contract) is bound to _two separate locations_ in the module
// tree: crate::lo::contract and crate::hi::contract, each of which has a (lo or
// hi) version-specific definition of stellar_env_host. We therefore
// import it from our _parent_ module rather than from the crate root.
use super::soroban_env_host::{
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
        LedgerEntryExt, Limits, ReadXdr, ScError, ScErrorCode, ScErrorType, ScSymbol, ScVal, TtlEntry,
        WriteXdr, XDR_FILES_SHA256,
    },
    HostError, LedgerInfo,
};
use std::error::Error;

impl From<CxxLedgerInfo> for LedgerInfo {
    fn from(c: CxxLedgerInfo) -> Self {
        Self {
            protocol_version: c.protocol_version,
            sequence_number: c.sequence_number,
            timestamp: c.timestamp,
            network_id: c.network_id.try_into().unwrap(),
            base_reserve: c.base_reserve,
            min_temp_entry_ttl: c.min_temp_entry_ttl,
            min_persistent_entry_ttl: c.min_persistent_entry_ttl,
            max_entry_ttl: c.max_entry_ttl,
        }
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

impl From<CxxRentFeeConfiguration> for RentFeeConfiguration {
    fn from(value: CxxRentFeeConfiguration) -> Self {
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

// FIXME: plumb this through from the limit xdrpp uses.
// Currently they are just two same-valued constants.
const MARSHALLING_STACK_LIMIT: u32 = 1000;

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
        Limits {depth: MARSHALLING_STACK_LIMIT, len: 0x1000000},
    ))
    // We only expect this to be called for safe, internal conversions, so this
    // should never happen.
    .map_err(|_| (ScErrorType::Value, ScErrorCode::InternalError))?)
}

fn non_metered_xdr_to_rust_buf<T: WriteXdr>(t: &T) -> Result<RustBuf, HostError> {
    let mut vec: Vec<u8> = Vec::new();
    t.write_xdr(&mut xdr::Limited::new(
        Cursor::new(&mut vec),
        Limits {depth: MARSHALLING_STACK_LIMIT, len: 0x1000000},
    ))
    .map_err(|_| (ScErrorType::Value, ScErrorCode::InvalidInput))?;
    Ok(vec.into())
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
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    ttl_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
    rent_fee_configuration: CxxRentFeeConfiguration,
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

fn invoke_host_function_or_maybe_panic(
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
) -> Result<InvokeHostFunctionOutput, Box<dyn Error>> {
    #[cfg(feature = "tracy")]
    let client = tracy_client::Client::start();
    let _span0 = tracy_span!("invoke_host_function_or_maybe_panic");

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
    let (res, time_nsecs) = {
        let _span1 = tracy_span!("e2e_invoke::invoke_function");
        let start_time = Instant::now();
        let res = e2e_invoke::invoke_host_function(
            &budget,
            enable_diagnostics,
            hf_buf,
            resources_buf,
            source_account_buf,
            auth_entries.iter(),
            ledger_info.into(),
            ledger_entries.iter(),
            ttl_entries.iter(),
            base_prng_seed,
            &mut diagnostic_events,
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
                    diagnostic_events: encode_diagnostic_events(&diagnostic_events),
                    cpu_insns,
                    mem_bytes,
                    time_nsecs,

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
    debug!(target: TX, "invocation failed: {}", err);
    return Ok(InvokeHostFunctionOutput {
        success: false,
        diagnostic_events: encode_diagnostic_events(&diagnostic_events),
        cpu_insns,
        mem_bytes,
        time_nsecs,

        result_value: vec![].into(),
        modified_ledger_entries: vec![],
        contract_events: vec![],
        rent_fee: 0,
    });
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
    let changed_entries = changed_entries.iter().map(|e| e.into()).collect();
    host_compute_rent_fee(&changed_entries, &fee_config.into(), current_ledger_seq)
}

pub(crate) fn compute_write_fee_per_1kb(
    bucket_list_size: i64,
    fee_config: CxxWriteFeeConfiguration,
) -> i64 {
    host_compute_write_fee_per_1kb(bucket_list_size, &fee_config.into())
}
