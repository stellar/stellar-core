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

    // ===== Soroban apply-phase types =====
    //
    // These are the per-TX inputs and outputs of the new Rust-owned Soroban
    // parallel-apply phase (see apply_soroban_phase below). Skeleton in C6;
    // the per-TX driver and orchestrator implementations land in C7..C9.

    // A single ledger-entry diff produced by the apply phase. Used for the
    // accumulated outputs (Rust → C++) that C++ writes to buckets after the
    // phase. An empty `value_xdr` means "delete this key".
    struct LedgerEntryUpdate {
        // XDR-serialized LedgerKey.
        key_xdr: RustBuf,
        // XDR-serialized LedgerEntry. Empty Vec = deletion of `key_xdr`.
        value_xdr: RustBuf,
    }

    // C++ → Rust prefetch entry. Same shape as `LedgerEntryUpdate` but
    // owned by C++ so we can move the freshly-`xdr_to_opaque`'d
    // `std::vector<uint8_t>` into a `unique_ptr` and ship without the
    // per-byte `push_back` loop cxx's `rust::Vec<u8>` forces on the
    // way in. The two structs are kept separate because cxx requires
    // bridge struct fields to be a fixed type, and a single struct
    // can't carry both `RustBuf` and `CxxBuf` cells.
    struct LedgerEntryInput {
        key_xdr: CxxBuf,
        value_xdr: CxxBuf,
    }

    // Per-entry delta produced by a single Soroban TX. C++ uses these
    // to build LedgerEntryChanges for the per-op meta:
    //
    //   - prev_value_xdr empty + new_value_xdr non-empty  → CREATED
    //   - prev_value_xdr non-empty + new_value_xdr empty  → STATE + REMOVED
    //   - both non-empty                                  → STATE + UPDATED
    //
    // RESTORED reclassification (for entries pulled out of the hot
    // archive or live bucket list with expired TTL) is layered on top
    // by C++ via processOpLedgerEntryChanges using the restored-key
    // hints already present in the host output; no extra bridge field
    // is needed for the basic CREATED/UPDATED/REMOVED shape.
    struct LedgerEntryDelta {
        key_xdr: RustBuf,
        prev_value_xdr: RustBuf,
        new_value_xdr: RustBuf,
    }

    // Per-TX outcome from the Soroban apply phase.
    struct SorobanTxApplyResult {
        success: bool,
        is_internal_error: bool,
        // True when the host succeeded but the TX's refundable-fee
        // budget cannot cover the host's rent_fee. Rust drops the
        // TX's writes / tx_changes when this is set; C++ uses the
        // flag to surface INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE
        // (or the equivalent for ExtendFootprintTtl / RestoreFootprint)
        // instead of the generic TRAPPED / MALFORMED codes.
        is_insufficient_refundable_fee: bool,
        // True when the TX hit a declared resource cap (currently only
        // disk read bytes; the host enforces instructions / memory and
        // surfaces those via is_internal_error or its own diagnostics).
        // C++ uses the flag to surface
        // INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED on the failure
        // path. Rust drops the TX's writes when this is set.
        is_resource_limit_exceeded: bool,
        // True when the TX touched a Soroban entry that was archived
        // (TTL expired and not auto-restored). Mirrors the legacy
        // doApply path's pre-host archival walk: persistent expired
        // entries fail with INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED rather
        // than the generic TRAPPED. Temporary expired entries are
        // *not* archival failures — they're simply skipped from the
        // host's footprint inputs, so the host treats the lookup as
        // missing.
        is_entry_archived: bool,
        // XDR-serialized return value (SCVal) for InvokeHostFunction TXs.
        // Empty for ExtendFootprintTtl / RestoreFootprint and for failed
        // TXs. Used by C++ to populate InvokeHostFunctionResult on
        // success and to compute the success-hash preimage.
        return_value_xdr: RustBuf,
        // XDR-serialized ContractEvents emitted by the host. Used both
        // to populate transaction meta and as part of the
        // InvokeHostFunctionSuccessPreImage hash. Empty for
        // ExtendFootprintTtl / RestoreFootprint and for failed TXs.
        contract_events: Vec<RustBuf>,
        // XDR-serialized DiagnosticEvents (ContractEvent + success
        // flag). Always populated when diagnostics are enabled,
        // regardless of success.
        diagnostic_events: Vec<RustBuf>,
        // Refundable-fee components consumed by this TX, used by C++ to
        // drive RefundableFeeTracker on success. Both are 0 on failure
        // (the C++ side resets the tracker via setInnermostError).
        //   * rent_fee_consumed: rent paid for state archival /
        //     extension. InvokeHostFunction gets this from the host;
        //     ExtendFootprintTtl and RestoreFootprint compute it from
        //     the rent-fee config.
        //   * contract_event_size_bytes: total XDR-serialised size of
        //     the contract events emitted by InvokeHostFunction (zero
        //     for the TTL ops since they emit no contract events).
        rent_fee_consumed: i64,
        contract_event_size_bytes: u32,
        // Per-TX entry deltas in apply order. Used by C++ to populate
        // the per-op LedgerEntryChanges meta. Empty for failed TXs.
        tx_changes: Vec<LedgerEntryDelta>,
        // Hot-archive restorations performed by this TX, indexed by
        // LedgerKey. Each entry is the value at the moment of
        // restoration (data/code from the archive, TTL freshly built).
        // Used by C++'s processOpLedgerEntryChanges to reclassify
        // CREATED → RESTORED for resurrected entries. Includes both the
        // data/code key and the matching TTL key.
        hot_archive_restores: Vec<LedgerEntryUpdate>,
        // Live-BucketList restorations (entries whose TTL had expired
        // but whose data/code still lived in the live BL). The data/
        // code entry is *not* modified by RestoreFootprint here — only
        // the TTL is bumped — so this map carries the unchanged live
        // value of the data/code key plus the new TTL.
        live_restores: Vec<LedgerEntryUpdate>,
        // Pre-computed SHA-256 of
        // `InvokeHostFunctionSuccessPreImage{returnValue, contractEvents}`,
        // used by C++ to populate
        // `InvokeHostFunctionResult.success`. The preimage XDR is
        // built by concatenating the host's already-encoded
        // `return_value_xdr`, a 4-byte big-endian event count, and
        // each `contract_events[i]` byte vec — no per-tx decode +
        // re-encode round-trip on the C++ side. Empty for non-
        // InvokeHostFunction success paths and for failures.
        success_preimage_hash: RustBuf,
        // Pre-computed events-portion of the resource fee for this TX
        // (the `refundable_fee` field of the host's
        // compute_transaction_resource_fee output, which depends on
        // the tx's resources + emitted events size). The C++ post-
        // pass adds this to `rent_fee_consumed` to populate the
        // RefundableFeeTracker without calling back through the
        // bridge to recompute the same value.
        refundable_fee_increment: i64,
    }

    // Aggregate result of a single Soroban parallel-apply phase. Returned
    // by apply_soroban_phase. The Soroban half of the writes is already
    // absorbed into the SorobanState passed in &mut; the four fields
    // below are what bucket persistence / LedgerTxn need to see.
    //
    // Soroban writes are pre-classified by Rust (which already knows
    // create vs update from `state.get(&k).is_some()` at fold time) so
    // the C++ post-pass can route them directly to the bucket
    // init/live/dead lists without the per-key XDR-decode + wasCreate
    // map walk the unsplit shape used to require.
    struct SorobanPhaseResult {
        per_tx: Vec<SorobanTxApplyResult>,
        // Soroban entries written for the first time this ledger (no
        // prior version in SorobanState). Each `RustBuf` is the host's
        // `metered_write_xdr` of the LedgerEntry with
        // `lastModifiedLedgerSeq` patched to the current ledger seq.
        // The matching `LedgerKey` is derivable from the entry via
        // `getLedgerKey` on the C++ side (see `InternalLedgerEntry`),
        // so we don't ship it over the bridge — saves ~50-100 bytes ×
        // ~6000 writes per phase plus the matching encode/decode.
        soroban_init_entry_xdrs: Vec<RustBuf>,
        // Soroban entries that updated a pre-existing version. Same
        // shape as `soroban_init_entry_xdrs`.
        soroban_live_entry_xdrs: Vec<RustBuf>,
        // Soroban keys whose entries were deleted this ledger.
        // XDR-serialized `LedgerKey` per element — value bytes have no
        // meaning for deletes so we don't ship a wrapper struct.
        soroban_dead_key_xdrs: Vec<RustBuf>,
        // Classic entries (Account / Trustline / etc.) emitted as side
        // effects of native asset operations executed by Soroban. The
        // C++ post-pass routes them through LedgerTxn for bucket
        // writeback because the classic invariants need to see them.
        classic_updates: Vec<LedgerEntryUpdate>,
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
        pub max_contract_size_bytes: u32,
        pub max_contract_data_entry_size_bytes: u32,
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
        disk_read_entries: u32,
        write_entries: u32,
        disk_read_bytes: u32,
        write_bytes: u32,
        contract_events_size_bytes: u32,
        transaction_size_bytes: u32,
    }

    struct CxxFeeConfiguration {
        fee_per_instruction_increment: i64,
        fee_per_disk_read_entry: i64,
        fee_per_write_entry: i64,
        fee_per_disk_read_1kb: i64,
        fee_per_write_1kb: i64,
        fee_per_historical_1kb: i64,
        fee_per_contract_event_1kb: i64,
        fee_per_transaction_size_1kb: i64,
    }

    struct CxxLedgerEntryRentChange {
        is_persistent: bool,
        is_code_entry: bool,
        old_size_bytes: u32,
        new_size_bytes: u32,
        old_live_until_ledger: u32,
        new_live_until_ledger: u32,
    }

    #[derive(Debug)]
    struct CxxRentFeeConfiguration {
        fee_per_write_1kb: i64,
        fee_per_rent_1kb: i64,
        fee_per_write_entry: i64,
        persistent_rent_rate_denominator: i64,
        temporary_rent_rate_denominator: i64,
    }

    struct CxxRentWriteFeeConfiguration {
        state_target_size_bytes: i64,
        rent_fee_1kb_state_size_low: i64,
        rent_fee_1kb_state_size_high: i64,
        state_size_rent_fee_growth_factor: u32,
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

    // These are used as return code for the command line tool so we use a
    // higher value to avoid collision. Note rust bridge translates these into
    // uint8_t, so make sure the values <= 255.
    enum QuorumCheckerStatus {
        UNSAT = 0,
        SAT = 101,
        UNKNOWN = 102,
    }

    struct QuorumSplit {
        left: Vec<String>,
        right: Vec<String>,
    }

    struct QuorumCheckerResource {
        time_ms: u64,
        mem_bytes: usize,
    }

    // The extern "Rust" block declares rust stuff we're going to export to C++.
    #[namespace = "stellar::rust_bridge"]
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn check_sensible_soroban_config_for_protocol(core_max_proto: u32);

        // Ed25519 signature verification using dalek library.
        // Returns true if signature is valid, false otherwise.
        // This function never throws/panics - it returns false for any invalid input.
        unsafe fn verify_ed25519_signature_dalek(
            public_key_ptr: *const u8,
            signature_ptr: *const u8,
            message_ptr: *const u8,
            message_len: usize,
        ) -> bool;
        fn invoke_host_function(
            config_max_protocol: u32,
            enable_diagnostics: bool,
            instruction_limit: u32,
            hf_buf: &CxxBuf,
            resources: CxxBuf,
            restored_rw_entry_indices: &Vec<u32>,
            source_account: &CxxBuf,
            auth_entries: &Vec<CxxBuf>,
            ledger_info: &CxxLedgerInfo,
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
        fn get_test_contract_sac_transfer(protocol_version: u32) -> Result<RustBuf>;
        fn get_write_bytes() -> Result<RustBuf>;
        fn get_invoke_contract_wasm() -> Result<RustBuf>;
        fn get_apply_load_token_wasm() -> Result<RustBuf>;
        fn get_apply_load_soroswap_factory_wasm() -> Result<RustBuf>;
        fn get_apply_load_soroswap_pool_wasm() -> Result<RustBuf>;
        fn get_apply_load_soroswap_router_wasm() -> Result<RustBuf>;

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

        // Exposes Rust's platform-compatible method for getting the full
        // filesystem path of the current running executable.
        fn current_exe() -> Result<String>;

        // Use Rust's superior-to-raw-libunwind backtrace machinery to
        // get a backtrace of the C++ caller's context.
        fn capture_cxx_backtrace() -> String;

        // Return the soroban versions linked into this binary. Panics
        // if the protocol version is not supported.
        fn get_soroban_version_info(core_max_proto: u32) -> Vec<SorobanVersionInfo>;

        // Check to see if the XDR files used by different rust dependencies match.
        fn check_xdr_version_identities() -> Result<()>;

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
        fn compute_rent_write_fee_per_1kb(
            config_max_protocol: u32,
            protocol_version: u32,
            bucket_list_size: i64,
            fee_config: CxxRentWriteFeeConfiguration,
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

        // Computes in-memory size of the ContractCodeEntry used for the rent
        // fee computation.
        // In-memory size is only used for contract code starting from protocol
        // 23, so it's an error to call this in the earlier protocols.
        fn contract_code_memory_size_for_rent(
            config_max_protocol: u32,
            protocol_version: u32,
            contract_code_entry: &CxxBuf,
            cpu_cost_params: &CxxBuf,
            mem_cost_params: &CxxBuf,
        ) -> Result<u32>;

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

        fn i128_is_negative(val: &CxxI128) -> Result<bool>;

        fn i128_i64_eq(lhs: &CxxI128, rhs: i64) -> Result<bool>;

        type SorobanModuleCache;

        fn new_module_cache() -> Result<Box<SorobanModuleCache>>;
        fn compile(self: &SorobanModuleCache, ledger_protocol: u32, source: &[u8]) -> Result<()>;
        fn shallow_clone(self: &SorobanModuleCache) -> Result<Box<SorobanModuleCache>>;
        fn evict_contract_code(self: &SorobanModuleCache, key: &[u8]) -> Result<()>;
        fn clear(self: &SorobanModuleCache) -> Result<()>;
        fn contains_module(self: &SorobanModuleCache, protocol: u32, key: &[u8]) -> Result<bool>;
        fn get_mem_bytes_consumed(self: &SorobanModuleCache, protocol: u32) -> Result<u64>;

        // SorobanState — canonical in-memory Soroban state (CONTRACT_DATA,
        // CONTRACT_CODE, TTL), owned by Rust. Replaces the C++
        // InMemorySorobanState class. The C++ shim drives this via the FFI
        // methods below. See src/rust/src/soroban_apply.rs for the typed
        // Rust API and design notes.
        //
        // All `_xdr` methods take serialized XDR bytes (as &CxxBuf). The
        // bytes are deserialized into the canonical (latest) stellar-xdr
        // type and dispatched to the typed implementation.
        type SorobanState;

        fn new_soroban_state() -> Box<SorobanState>;

        // Reads. lookup_entry_xdr returns an empty RustBuf when the key is
        // not present (a real LedgerEntry is never an empty byte sequence).
        fn lookup_entry_xdr(self: &SorobanState, key_xdr: &CxxBuf) -> RustBuf;
        fn has_ttl_xdr(self: &SorobanState, key_xdr: &CxxBuf) -> bool;

        // Trivial accessors (mirror the read-only C++ InMemorySorobanState
        // public API).
        fn is_empty(self: &SorobanState) -> bool;
        fn ledger_seq(self: &SorobanState) -> u32;
        fn size(self: &SorobanState) -> u64;
        fn contract_data_entry_count(self: &SorobanState) -> usize;
        fn contract_code_entry_count(self: &SorobanState) -> usize;

        // Lifecycle / invariants.
        fn manually_advance_ledger_header(self: &mut SorobanState, ledger_seq: u32);
        fn check_update_invariants(self: &SorobanState);
        fn assert_last_closed_ledger(self: &SorobanState, expected_ledger_seq: u32);

        // CRUD — ContractData. `entry_xdr` must be a serialized
        // CONTRACT_DATA LedgerEntry; `key_xdr` must be a serialized
        // CONTRACT_DATA LedgerKey. Each method panics on a type mismatch
        // (mirrors the C++ releaseAssertOrThrow checks).
        fn create_contract_data_entry_xdr(self: &mut SorobanState, entry_xdr: &CxxBuf);
        fn update_contract_data_xdr(self: &mut SorobanState, entry_xdr: &CxxBuf);
        fn delete_contract_data_xdr(self: &mut SorobanState, key_xdr: &CxxBuf);

        // CRUD — ContractCode. The caller (C++ shim) computes
        // protocol-aware size_bytes via the existing
        // ledgerEntrySizeForRent path and passes it in. Storage doesn't try
        // to recompute it.
        fn create_contract_code_entry_xdr(
            self: &mut SorobanState,
            entry_xdr: &CxxBuf,
            size_bytes: u32,
        );
        fn update_contract_code_xdr(
            self: &mut SorobanState,
            entry_xdr: &CxxBuf,
            size_bytes: u32,
        );
        fn delete_contract_code_xdr(self: &mut SorobanState, key_xdr: &CxxBuf);

        // CRUD — TTL. `entry_xdr` must be a serialized TTL LedgerEntry.
        fn create_ttl_xdr(self: &mut SorobanState, entry_xdr: &CxxBuf);
        fn update_ttl_xdr(self: &mut SorobanState, entry_xdr: &CxxBuf);

        // Notify SorobanState of post-apply eviction events: archived
        // entries (data/code moved to the hot archive) and deleted
        // keys (TTLs of evicted entries plus expired-temporary data
        // entries). Removes the corresponding CONTRACT_DATA /
        // CONTRACT_CODE entries from the in-memory map; TTL keys and
        // non-Soroban keys are no-ops. Lenient on missing entries.
        fn evict_entries_xdr(
            self: &mut SorobanState,
            archived_entry_keys: &Vec<CxxBuf>,
            deleted_keys: &Vec<CxxBuf>,
        );

        // Batch-apply init / live / dead entries to SorobanState in one
        // call. Used by the BucketTestUtils replay path that bypasses
        // the normal apply phase (setNextLedgerEntryBatchForBucketTesting
        // flow) — entries flow into the live BucketList via
        // addLiveBatch, so we need to mirror them into SorobanState too
        // or post-apply paths (eviction lookup, etc.) won't see them.
        // Soroban-only: classic / config entries are ignored.
        fn batch_update_xdr(
            self: &mut SorobanState,
            init_entries: &Vec<CxxBuf>,
            live_entries: &Vec<CxxBuf>,
            dead_keys: &Vec<CxxBuf>,
            new_ledger_seq: u32,
            ledger_version: u32,
            config_max_protocol: u32,
            cpu_cost_params: &CxxBuf,
            mem_cost_params: &CxxBuf,
        );

        // Reset the state to empty. Used by the C++ shim's clearForTesting
        // path. Cheaper than dropping the Box and constructing a new one.
        fn clear(self: &mut SorobanState);

        // Recompute the cached size_bytes for every stored CONTRACT_CODE
        // entry. Used during protocol upgrades when the in-memory size
        // computation changes. Done entirely on the Rust side: the iteration
        // is a single FFI call, and the per-entry size computation reaches
        // into the per-protocol soroban-env-host directly via
        // contract_code_memory_size_for_rent_bytes (no C++ round-trip).
        fn recompute_contract_code_size_xdr(
            self: &mut SorobanState,
            config_max_protocol: u32,
            protocol_version: u32,
            cpu_cost_params: &CxxBuf,
            mem_cost_params: &CxxBuf,
        );

        // Initialize SorobanState from a list of live-bucket file paths in
        // priority order (level 0 curr, level 0 snap, level 1 curr, level 1
        // snap, ...). Replaces the old C++ initializeStateFromSnapshot path:
        // the bucket-list iteration, dedup against DEADENTRY records, and
        // contract-code size compute all happen entirely on the Rust side.
        // The state must be empty when called.
        //
        // Pre-Soroban protocols are a no-op (just sets the ledger seq).
        fn initialize_from_bucket_files(
            self: &mut SorobanState,
            bucket_paths: &Vec<String>,
            last_closed_ledger_seq: u32,
            ledger_version: u32,
            config_max_protocol: u32,
            cpu_cost_params: &CxxBuf,
            mem_cost_params: &CxxBuf,
        );

        // Run the entire Soroban parallel-apply phase for one ledger.
        // Replaces the C++ applySorobanStages + ParallelApplyUtils
        // orchestration. The full per-TX driver, stage orchestrator, and
        // per-protocol host dispatch implementation lands in C7..C9; this
        // declaration is the bridge skeleton (returns an error in C6).
        //
        // Inputs:
        //   - state: SorobanState, mutated in place. Soroban-state diffs from
        //     this phase are absorbed into `state` before return; the
        //     returned `ledger_updates` Vec is what bucket persistence
        //     should write.
        //   - module_cache: parsed-WASM cache, shared across phases.
        //   - soroban_phase_xdr: the Soroban portion of the closing
        //     ledger's TxSet, XDR-serialized. Cluster/stage structure is
        //     baked into the TxSet — Rust does not re-cluster.
        //   - classic_prefetch: classic-state entries (source accounts,
        //     etc.) the Soroban TXs touch, pre-loaded by C++ from the
        //     LedgerTxn / bucket list.
        //   - archived_prefetch: hot-archive entries pre-loaded by C++ for
        //     RestoreFootprint operations.
        //   - ledger_info: closing ledger header info.
        //   - rent_fee_configuration: per-ledger rent-fee parameters.
        //   - cpu_cost_params / mem_cost_params: serialized
        //     ContractCostParams from SorobanNetworkConfig.
        fn apply_soroban_phase(
            state: &mut SorobanState,
            module_cache: &SorobanModuleCache,
            config_max_protocol: u32,
            // Flat list of TransactionEnvelope XDR bytes in apply
            // order — C++ no longer wraps them in a TransactionPhase
            // before encoding; Rust deserializes each in parallel
            // across the cluster worker pool.
            soroban_envelopes: &Vec<CxxBuf>,
            // Per-cluster TX count and per-stage cluster count let
            // Rust rebuild the stage / cluster structure out of the
            // flat envelopes vec.
            soroban_cluster_sizes: &Vec<u32>,
            soroban_stage_cluster_counts: &Vec<u32>,
            // 32-byte base seed — typically the txset's content hash.
            // Per-TX seeds are derived as
            // SHA256(soroban_base_prng_seed || tx_num_be) where tx_num is
            // the TX's apply-order index in the phase.
            soroban_base_prng_seed: &CxxBuf,
            classic_prefetch: &Vec<LedgerEntryInput>,
            archived_prefetch: &Vec<LedgerEntryInput>,
            ledger_info: &CxxLedgerInfo,
            rent_fee_configuration: CxxRentFeeConfiguration,
            // Per-TX max_refundable_fee, in apply-order matching the
            // soroban_phase_xdr's stage/cluster/tx walk (declared
            // resource fee minus non_refundable_fee). When the host's
            // returned rent_fee for a TX exceeds this cap, Rust drops
            // the TX's writes from cluster_local_writes / tx_changes
            // and signals failure back to C++ via the tx result.
            per_tx_max_refundable_fee: &Vec<i64>,
            // When false, the host runs with diagnostics off and the
            // orchestrator skips emitting the per-tx core_metrics
            // diagnostic events. Mirrors the legacy
            // `cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS` gate that the
            // C++ op-frame consulted before calling
            // `maybePopulateMetricsInDiagnosticEvents`.
            enable_diagnostics: bool,
            // When false, the orchestrator skips populating per-tx
            // `tx_changes` (LedgerEntryDelta) and the contract-events
            // / return-value byte buffers used only by the C++ meta
            // builder. Hot-archive / live restore tracking still runs
            // because the post-pass markRestoredFrom* calls consume
            // it independently of the meta gate.
            enable_tx_meta: bool,
            // Per-ledger fee config (`compute_transaction_resource_fee`
            // input) plus per-tx envelope byte size table. Lets the
            // orchestrator precompute each tx's
            // `refundable_fee_increment` (events portion of the
            // SorobanResources fee, used by the C++
            // RefundableFeeTracker) inside the cluster worker — saves
            // 6000 round-trip FFI calls into the bridge from the C++
            // post-pass.
            fee_configuration: CxxFeeConfiguration,
            per_tx_envelope_size_bytes: &Vec<u32>,
        ) -> Result<SorobanPhaseResult>;

        // Given a quorum set configuration, checks if quorum intersection is
        // enjoyed among all possible quorums. Returns `Ok(status)` where
        // `status` can be:
        //  - `QuorumCheckerStatus::UNSAT` if enjoys quorum intersection (good!)
        //  - `QuorumCheckerStatus::SAT` if found quorum split (bad!). It also
        //     populates `potential_split` with the split found (may not be
        //     unique).
        //  - `QuorumCheckerStatus::UNKNOWN`. Solver could not finish, possibly
        //     due to reaching some internal limits (e.g. num conflicts) not
        //     including resource limits (see "Resource limits and errors")
        //
        // Resource limits and errors:
        //
        // The quorum checker accepts two limits (passed via `resource_limit`),
        // time (ms) and memory (bytes). The time limit is enforced internally
        // via code logic, once exceeds, returns a solver error. The memory
        // limit is enforced by a global memory allocator, and if exceeded, will
        // abort the program. In other words, memory limit is a hard, system
        // enforced limit.
        //
        // Errors:
        //  - if resource limits (not including memory) have been exceeded.
        //  - any other solver error In either sucess or error case, the
        // `resource_usage` will be updated with the actual resource
        // consumption.
        //
        // Aborts:
        // - if the memory limit has been exceeded
        // Abort is non-recoverable (it cannot be caught by catch_unwind)
        fn network_enjoys_quorum_intersection(
            nodes: &Vec<CxxBuf>,
            quorum_set: &Vec<CxxBuf>,
            potential_split: &mut QuorumSplit,
            resource_limit: &QuorumCheckerResource,
            resource_usage: &mut QuorumCheckerResource,
        ) -> Result<QuorumCheckerStatus>;

        // The QI checker actually manages the memory limit using a global
        // allocator, which winds up controlling _all_ memory allocation by
        // rust code in the process. So we want to ensure that limit is unlimited
        // when the process starts up -- the QI check call will limit it later,
        // if and only if it's running as a QI-checking subprocess.
        fn set_rust_global_memory_limit_to_unlimited();
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
        unsafe fn shim_copyU8Vector(data: *const u8, len: usize) -> UniquePtr<CxxVector<u8>>;
    }
}

// Because this file is processed with cxx.rs, we need to "use" the
// definitions of all the symbols we declare above, just to make the
// resulting code compile -- they all get referenced in the generated
// code so they have to be in scope here.
use crate::b64::*;
use crate::common::*;
use crate::ed25519_verify::*;
use crate::i128::*;
use crate::log::*;
use crate::quorum_checker::*;
use crate::soroban_apply::*;
use crate::soroban_invoke::*;
use crate::soroban_module_cache::*;
use crate::soroban_proto_all::*;
use crate::soroban_test_wasm::*;
