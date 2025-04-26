// This module contains helper code to assist in comparing two versions of
// soroban against one another by running the same transaction twice on
// different hosts, and comparing its output for divergence or changes to costs.
// All this functionality is gated by the "testutils" feature.

use std::hash::Hasher;
use std::str::FromStr;

use crate::{
    log::partition::TX,
    soroban_proto_all::{get_host_module_for_protocol, p22, HostModule},
    CxxBuf, CxxLedgerInfo, CxxRentFeeConfiguration, InvokeHostFunctionOutput, RustBuf,
    SorobanModuleCache,
};
use log::{info, warn};

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
    module_cache: &SorobanModuleCache,
) {
    if let Ok(extra) = std::env::var("SOROBAN_TEST_EXTRA_PROTOCOL") {
        if let Ok(proto) = u32::from_str(&extra) {
            info!(target: TX, "comparing soroban host for protocol {} with {}", ledger_info.protocol_version, proto);
            if let Ok(hm2) = get_host_module_for_protocol(proto, proto) {
                if let Err(e) = modify_ledger_info_for_extra_test_execution(&mut ledger_info, proto)
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
                    module_cache,
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
            use p22::soroban_proto_any::{
                inplace_modify_cxxbuf_encoded_type, xdr::SorobanResources,
            };
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
