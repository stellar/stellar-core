use crate::{
    soroban_proto_all::get_host_module_for_protocol, CxxBuf, CxxFeeConfiguration,
    CxxLedgerEntryRentChange, CxxLedgerInfo, CxxRentFeeConfiguration, CxxRentWriteFeeConfiguration,
    CxxTransactionResources, FeePair, InvokeHostFunctionOutput, SorobanModuleCache,
};

pub(crate) fn invoke_host_function(
    config_max_protocol: u32,
    enable_diagnostics: bool,
    instruction_limit: u32,
    hf_buf: &CxxBuf,
    resources_buf: CxxBuf,
    restored_rw_entry_indices: &Vec<u32>,
    source_account_buf: &CxxBuf,
    auth_entries: &Vec<CxxBuf>,
    ledger_info: CxxLedgerInfo,
    ledger_entries: &Vec<CxxBuf>,
    ttl_entries: &Vec<CxxBuf>,
    base_prng_seed: &CxxBuf,
    rent_fee_configuration: CxxRentFeeConfiguration,
    module_cache: &SorobanModuleCache,
) -> Result<InvokeHostFunctionOutput, Box<dyn std::error::Error>> {
    use std::error::Error as StdError;
    type BoxStdErr = Box<dyn StdError>;
    type BoxStdErrSend = Box<dyn StdError + Send>;
    type BoxStdErrSendSync = Box<dyn StdError + Send + Sync>;

    fn sendable_str_err(str: &str) -> BoxStdErrSend {
        let tmp: BoxStdErrSendSync = Box::from(str);
        tmp as BoxStdErrSend
    }

    let hm = get_host_module_for_protocol(config_max_protocol, ledger_info.protocol_version)?;
    // Rust stacks are 2MiB by default, which is a little too small
    // for comfort; to give ourselves a little more breathing room
    // against unforeseen bugs we use a 100MiB stack. Unfortunately
    // there's no easy way to enforce this at the C++ side when the
    // initial std::async parallel-exec thread is spawned, so we
    // have to spawn _another_ here. On linux this is fairly fast,
    // on the order of a ten-ish microseconds.
    let LARGE_STACK_SIZE: usize = 100 * 1024 * 1024; // 100 MiB
    let res = std::thread::scope(|scope| {
        std::thread::Builder::new()
            .stack_size(LARGE_STACK_SIZE)
            .spawn_scoped(scope, || {
                (hm.invoke_host_function)(
                    enable_diagnostics,
                    instruction_limit,
                    hf_buf,
                    &resources_buf,
                    restored_rw_entry_indices,
                    source_account_buf,
                    auth_entries,
                    &ledger_info,
                    ledger_entries,
                    ttl_entries,
                    base_prng_seed,
                    &rent_fee_configuration,
                    module_cache,
                )
                // Map non-sendable error to sendable for crossing thread boundary.
                // This is crude but the error is going to be stringified on the
                // bridge-crossing anyways.
                .map_err(|e| sendable_str_err(&format!("{e}")))
            })
            .map_err(|_| sendable_str_err("spawn_scoped failed"))?
            .join()
            .map_err(|_| sendable_str_err("join failed"))?
    });

    // Map sendable error back to non-sendable -- Rust doesn't do dyn upcasts.
    let res = res.map_err(|e: BoxStdErrSend| e as BoxStdErr);

    #[cfg(feature = "testutils")]
    crate::soroban_test_extra_protocol::maybe_invoke_host_function_again_and_compare_outputs(
        &res,
        &hm,
        config_max_protocol,
        enable_diagnostics,
        instruction_limit,
        hf_buf,
        resources_buf,
        restored_rw_entry_indices,
        source_account_buf,
        auth_entries,
        ledger_info,
        ledger_entries,
        ttl_entries,
        base_prng_seed,
        rent_fee_configuration,
        module_cache,
    );

    res
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

pub(crate) fn compute_rent_write_fee_per_1kb(
    config_max_protocol: u32,
    protocol_version: u32,
    bucket_list_size: i64,
    fee_config: CxxRentWriteFeeConfiguration,
) -> Result<i64, Box<dyn std::error::Error>> {
    let hm = get_host_module_for_protocol(config_max_protocol, protocol_version)?;
    Ok((hm.compute_rent_write_fee_per_1kb)(
        bucket_list_size,
        fee_config,
    ))
}
