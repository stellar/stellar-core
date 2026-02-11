---
name: subsystem-summary-of-soroban-env
description: "read this skill for a token-efficient summary of the soroban-env subsystem"
---

# Soroban Env Subsystem (p26) — Technical Summary

## Overview

The Soroban environment subsystem is split into two crates: `soroban-env-common` and `soroban-env-host`. Together they define the host-guest interface for Soroban smart contracts. `soroban-env-common` defines the ABI types and trait interfaces shared between guest (Wasm) and host code. `soroban-env-host` provides the concrete `Host` implementation that executes contracts, manages storage, budgets, authorization, events, and the Wasm VM.

The p26 host only supports protocol version 26 and later (`MIN_LEDGER_PROTOCOL_VERSION = 26`).

---

## soroban-env-common

### Val — The Universal 64-bit Value Type

`Val` (`val.rs`) is a 64-bit (`u64`) union type that is the fundamental ABI type crossing the host-guest boundary. It uses bit-packing:

- **Low 8 bits**: `Tag` enum indicating the type.
- **Upper 56 bits**: `body`, optionally subdivided into a 32-bit `major` and 24-bit `minor`.

**Tag categories:**
- **Small tags (0–14)**: Values packed entirely within the 56-bit body — `False`, `True`, `Void`, `Error`, `U32Val`, `I32Val`, `U64Small`, `I64Small`, `TimepointSmall`, `DurationSmall`, `U128Small`, `I128Small`, `U256Small`, `I256Small`, `SymbolSmall`.
- **Object tags (64–78)**: Reference host-side objects via a 32-bit handle in the `major` field — `U64Object`, `I64Object`, `TimepointObject`, `DurationObject`, `U128Object`, `I128Object`, `U256Object`, `I256Object`, `BytesObject`, `StringObject`, `SymbolObject`, `VecObject`, `MapObject`, `AddressObject`, `MuxedAddressObject`.
- `Tag::Bad (0x7f)`: Sentinel for mis-tagged values.

Small values (numbers that fit in 56 bits, symbols ≤9 chars) avoid host object allocation. Larger values overflow to host objects transparently.

### Wrapper Types

Type-safe wrappers around `Val` that statically guarantee the tag:
- `Object` — any object-tagged Val; carries a 32-bit handle.
- `Symbol` / `SymbolSmall` / `SymbolObject` — identifiers restricted to `[a-zA-Z0-9_]`. `SymbolSmall` packs up to 9 chars into 54 bits using 6-bit codes. `SymbolStr` is a fixed-size buffer for extracting symbol bytes.
- `Error` — tag=3, encodes `(ScErrorType: 24-bit minor, ScErrorCode: 32-bit major)`.
- Numeric wrappers: `U32Val`, `I32Val`, `U64Val`/`U64Small`/`U64Object`, `I64Val`, `U128Val`, `I128Val`, `U256Val`, `I256Val`, `TimepointVal`, `DurationVal`, plus their Small/Object variants.
- `Bool`, `Void` — singleton-like wrappers.
- `BytesObject`, `StringObject`, `MapObject`, `VecObject`, `AddressObject`, `MuxedAddressObject`.

### Env and VmCallerEnv Traits

`EnvBase` (`env.rs`) — base trait with associated `Error` type, integrity checks, tracing hooks, and slice-passing helper methods (`bytes_copy_from_slice`, `bytes_new_from_slice`, `map_new_from_slices`, `vec_new_from_slice`, etc.). These bypass the Wasm ABI for trusted callers.

`Env` — generated via the `call_macro_with_all_host_functions!` x-macro from `env.json`. Declares all host functions that guest contracts can call. Each method takes and returns only 64-bit values (`Val` and wrappers). The x-macro allows the same function list to be reflected in multiple contexts (trait declaration, dispatch, function info tables).

`VmCallerEnv` (`vmcaller_env.rs`) — variant of `Env` where each method takes an additional `&mut VmCaller<Self::VmUserState>` parameter, allowing host function implementations to access the Wasm `Caller` context (e.g., for linear memory access). A blanket `impl Env for T where T: VmCallerEnv` passes `VmCaller::none()` automatically, so native callers don't need to deal with `VmCaller`.

### Convert and Compare Traits

`Convert<F, T>` — generic fallible conversion trait. `TryFromVal<E, V>` / `TryIntoVal<E, V>` — `Env`-aware conversion traits used to convert between Rust types and `Val` (e.g., `i64 <-> Val` goes through small-or-object path via the Env). `Compare<T>` — `Env`-aware ordering trait (needed because comparing objects requires host access).

### ConversionError

Minimal uninformative error for ubiquitous tag/number conversions in Wasm, converting to `Error(ScErrorType::Value, ScErrorCode::UnexpectedType)`.

### ScValObject / ScValObjRef

Helper types that classify which `ScVal` variants require host-side object storage vs. fitting into a small `Val`.

---

## soroban-env-host

### Host — The Core Runtime

`Host` (`host.rs`) is a newtype around `Rc<HostImpl>` implementing `VmCallerEnv` (and thus `Env`). It is the concrete environment that executes Soroban contracts. `HostImpl` is a `#[derive(Clone, Default)]` struct containing all mutable state behind `RefCell`s:

- `objects: Vec<HostObject>` — the host object table (indexed by absolute handles).
- `storage: Storage` — ledger entry access.
- `context_stack: Vec<Context>` — call stack of frames.
- `budget: Budget` — CPU/memory metering (Rc-shared, not deep-cloned).
- `events: InternalEventsBuffer` — contract and diagnostic events.
- `authorization_manager: AuthorizationManager` — auth tracking.
- `module_cache: Option<ModuleCache>` — cached parsed Wasm modules.
- `ledger: Option<LedgerInfo>` — current ledger metadata.
- `source_account: Option<AccountId>` — transaction source account.
- `base_prng: Option<Prng>` — seeded PRNG for deterministic randomness.
- `diagnostic_level: DiagnosticLevel` — controls debug event emission.
- `trace_hook: Option<TraceHook>` — lifecycle tracing callback.

Construction: `Host::with_storage_and_budget(storage, budget)` or `Host::default()`.

Finalization: `Host::try_finish()` consumes the host (requires refcount=1) and returns `(Storage, Events)`.

### HostObject — The Object Table

`HostObject` (`host_object.rs`) is an enum of all host-side object types:
`Vec(HostVec)`, `Map(HostMap)`, `U64(u64)`, `I64(i64)`, `TimePoint`, `Duration`, `U128`, `I128`, `U256`, `I256`, `Bytes(ScBytes)`, `String(ScString)`, `Symbol(ScSymbol)`, `Address(ScAddress)`, `MuxedAddress(MuxedScAddress)`.

`HostObjectType` trait: `inject(self, host) -> HostObject`, `try_extract(&HostObject) -> Option<&Self>`, `new_from_handle(u32) -> Wrapper`.

**Object handles** have two flavors:
- **Absolute** (odd low bit): index into `Host.objects`. Used by host code and stored in host objects.
- **Relative** (even low bit): per-frame indirection table index. Used by Wasm guest code. Translation happens at the VM boundary during dispatch.

Key methods:
- `add_host_object<HOT>(hot) -> HOT::Wrapper` — pushes into the object vec, returns handle.
- `visit_obj<HOT, F>(obj, f) -> U` — looks up object by handle, charges `VisitObject`, calls closure with `&HOT`.
- `relative_to_absolute(val)` / `absolute_to_relative(val)` — handle translation at VM boundary.

### Frame and Context — Call Stack

`Frame` (`host/frame.rs`) — enum of invocation types:
- `ContractVM { vm, fn_name, args, instance, relative_objects }` — Wasm contract call.
- `HostFunction(HostFunctionType)` — top-level host function invocation.
- `StellarAssetContract(ContractId, Symbol, Vec<Val>, ScContractInstance)` — built-in SAC.
- `TestContract(TestContractFrame)` — test-only.

`Context` wraps a `Frame` with optional per-frame `Prng` and `InstanceStorageMap`.

`RollbackPoint` captures `(StorageMap, events_len, AuthorizationManagerSnapshot)` for sub-transaction rollback.

**`Host::with_frame(frame, f)`** — the central frame lifecycle method. Pushes a context (capturing rollback point), runs closure, pops context. On error, rolls back storage and events. Handles `Ok(Error)` returns from contracts (converts to `Err`), distinguishing contract errors from spoofed system errors. Enforces depth limit (`DEFAULT_HOST_DEPTH_LIMIT`).

`ContractReentryMode`: `Prohibited`, `SelfAllowed`, `Allowed`.

### Storage — Ledger Access

`Storage` (`storage.rs`) mediates all ledger entry access with two modes:
- `FootprintMode::Recording(SnapshotSource)` — preflight mode, records accessed keys.
- `FootprintMode::Enforcing` — production mode, rejects accesses outside declared footprint.

Components:
- `Footprint(FootprintMap)` — `MeteredOrdMap<Rc<LedgerKey>, AccessType, Budget>` mapping keys to `ReadOnly`/`ReadWrite`.
- `StorageMap` — `MeteredOrdMap<Rc<LedgerKey>, Option<EntryWithLiveUntil>, Budget>` holding actual entries.
- `InstanceStorageMap` — in-memory per-contract instance storage (from `ScContractInstance.storage`), with `is_modified` flag.

Key operations: `get`, `try_get`, `put`, `del`, `has`, `get_with_live_until_ledger`. Each checks footprint first and delegates to the underlying map. TTL extension methods handle `extend_ttl` and `restore`.

Supported ledger entry types: `Account`, `Trustline`, `ContractData`, `ContractCode`.

### Budget — Metering System

`Budget` (`budget.rs`) is an `Rc<RefCell<BudgetImpl>>` tracking CPU instructions and memory bytes consumption. It uses a cost model based on `ContractCostType` enum variants.

`BudgetImpl` contains:
- `cpu_insns: BudgetDimension` — CPU budget with per-cost-type linear models (`const_term + lin_term * input`).
- `mem_bytes: BudgetDimension` — memory budget.
- `tracker: BudgetTracker` — per-cost-type iteration/input/cpu/mem counters.
- `is_in_shadow_mode: bool` — when true, charges are tracked but don't fail on exceeding limits (used for debug/diagnostic work).
- `fuel_costs: wasmi::FuelCosts` — calibrated Wasm fuel costs for wasmi.
- `depth_limit: u32` — recursion depth limit.

`Budget::charge(ty, input)` is the core metering call, invoked pervasively. It updates tracking, charges both CPU and memory dimensions, and checks limits. In shadow mode, limits aren't enforced.

`AsBudget` trait allows both `Budget` and `Host` to be used as budget references.

Fuel bridge: `get_wasmi_fuel_remaining()` converts remaining CPU budget to wasmi fuel units. Fuel is transferred to/from wasmi at host function call boundaries.

### Metered Data Structures

- `MeteredOrdMap<K, V, Ctx>` (`host/metered_map.rs`) — sorted `Vec<(K, V)>` with binary search. All operations (insert, get, delete) charge budget based on `DeclaredSizeForMetering`. Used for `HostMap`, `FootprintMap`, `StorageMap`.
- `MeteredVector<A>` (`host/metered_vector.rs`) — `Vec<A>` wrapper with metered insert/append/remove. Used for `HostVec`.
- `MeteredClone` trait (`host/metered_clone.rs`) — charges `MemCpy` budget for cloning, with `DeclaredSizeForMetering` providing stable size constants (not `size_of` which may vary). `charge_shallow_copy` and `charge_heap_alloc` are the underlying charging functions.
- `MeteredHash` (`host/metered_hash.rs`) — metered hashing.

### VM — Wasm Execution

`Vm` (`vm.rs`) wraps a `wasmi::Instance` for a single Wasm module:
- `contract_id: ContractId`
- `module: Arc<ParsedModule>`
- `wasmi_store: RefCell<wasmi::Store<Host>>`
- `wasmi_instance: wasmi::Instance`
- `wasmi_memory: Option<wasmi::Memory>`

Rejects modules with floating point or start functions.

`ParsedModule` (`vm/parsed_module.rs`) — pre-parsed, validated Wasm module. Stores `wasmi::Module`, `VersionedContractCodeCostInputs` (V0 = just byte length, V1 = detailed instruction/function/global counts), and imported symbol set. Charges parsing and instantiation costs separately.

`ModuleCache` (`vm/module_cache.rs`) — caches `Arc<ParsedModule>` keyed by code hash, shared across invocations within a host. Can be installed externally or built from host storage.

### Dispatch — Host Function Routing

`dispatch.rs` uses the `call_macro_with_all_host_functions!` x-macro to generate one dispatch function per host function. Each dispatch function:
1. Transfers wasmi fuel to host CPU budget (`FuelRefillable`).
2. Charges `DispatchHostFunction` cost.
3. Converts wasmi `i64` args to `Val`/wrappers (with relative-to-absolute object translation via `RelativeObjectConversion`).
4. Calls the `VmCallerEnv` method on Host.
5. Converts result back (absolute-to-relative).
6. Transfers residual CPU budget back to wasmi fuel.

`func_info.rs` — static `HOST_FUNCTIONS` array of `HostFuncInfo` structs (mod name, fn name, arity, wrap function, protocol bounds). Used by linker setup and introspection.

Protocol gating: each host function can have optional `min_proto`/`max_proto` bounds checked at dispatch time.

### Events

`events/mod.rs` — `HostEvent` wraps `ContractEvent` XDR with `failed_call` flag. `InternalEventsBuffer` (`events/internal.rs`) stores events during execution. Events are rolled back on frame failure. `system_events.rs` emits system events for contract lifecycle operations.

Event types: `Contract` (user-emitted), `System` (host-emitted lifecycle), `Diagnostic` (debug-only, guarded by `DiagnosticLevel::Debug` and shadow budget).

### Authorization

`AuthorizationManager` (`auth.rs`) handles both authorization (is action allowed?) and authentication (is credential authentic?). Operates in two modes:
- **Enforcing**: validates `SorobanAuthorizationEntry` trees against actual invocation patterns.
- **Recording**: captures auth requirements during preflight.

Address types for auth:
1. **Invoker contract** — implicit auth from call chain.
2. **Stellar account with credentials** — classic multisig to medium threshold.
3. **Transaction source account** — pre-authenticated by transaction signatures.
4. **Custom account contract** — delegates to `__check_auth` export.

`require_auth(Address)` is the main entrypoint. Matches `AuthorizedInvocation` trees against execution context. Each pattern node matches at most once per transaction.

### Contract Lifecycle

`host/lifecycle.rs` — `create_contract_internal` orchestrates:
1. Compute contract ID from preimage.
2. Verify Wasm code exists.
3. Store `ScContractInstance` entry.
4. Initialize SAC if asset-derived.
5. Call `__constructor` (protocol ≥22; missing constructor OK if 0 args).

### Conversion

`host/conversion.rs` — methods on `Host` for converting between `ScVal`/XDR types and `Val`/host objects. Includes `to_valid_host_val`, `from_host_val`, address/hash extraction helpers, and `ScMap`↔`HostMap` conversions. All operations are metered.

### Error Handling

`HostError` (`host/error.rs`) wraps an `Error` (the 64-bit `Val`-encoded error) plus optional `DebugInfo` (event log + backtrace). `ErrorHandler` trait provides `map_err` for wasmi error conversion. Errors are augmented with context via `augment_err_result`.

Recoverable vs non-recoverable: `ScErrorType::Contract` errors are recoverable (can be caught by `try_call`). All others (Budget, Internal, etc.) are non-recoverable and propagate up.

### e2e_invoke — Embedder Integration

`e2e_invoke.rs` provides the top-level entry point for executing Soroban host functions from embedders (stellar-core, RPC). Key types:
- `InvokeHostFunctionResult` — result, ledger changes, encoded events.
- `LedgerEntryChange` — per-entry diff with TTL change info.
- `get_ledger_changes()` — computes diff between post-execution storage and initial snapshot.

### Fees

`fees.rs` — fee computation protocol for Soroban, shared between stellar-core and RPC. Defines `TransactionResources`, `FeeConfiguration`, `RentWriteFeeConfiguration`, `LedgerEntryRentChange`. Computes resource fees (CPU, read/write entries/bytes, events, transaction size) and rent fees (based on state size target and TTL extension).

### Crypto

`crypto/mod.rs` — Ed25519 (sign/verify), ECDSA secp256k1/secp256r1, SHA-256, Keccak-256, BLS12-381 (`crypto/bls12_381.rs`), BN254 (`crypto/bn254.rs`), Poseidon/Poseidon2 hashes (`crypto/poseidon/`). All operations charge appropriate `ContractCostType` budget entries.

### Built-in Contracts

`builtin_contracts.rs` — `BuiltinContract` trait with `fn call(&self, func, host, args) -> Result<Val, HostError>`.

Key built-ins:
- **Stellar Asset Contract (SAC)** (`stellar_asset_contract/`) — wraps classic Stellar assets as Soroban contracts. Modules: `contract.rs` (main dispatch), `balance.rs`, `allowance.rs`, `admin.rs`, `event.rs`, `metadata.rs`, `storage_types.rs`, `asset_info.rs`, `public_types.rs`.
- **Account Contract** (`account_contract.rs`) — implements `__check_auth` for classic Stellar accounts.
- `invoker_contract_auth.rs` — handles invoker-contract auth entries.
- `storage_utils.rs`, `base_types.rs`, `common_types.rs` — shared utilities.

### LedgerInfo

`LedgerInfo` (`ledger_info.rs`) — protocol version, sequence number, timestamp, network ID, base reserve, TTL bounds. Methods: `min_live_until_ledger_checked`, `max_live_until_ledger_checked`.

### PRNG

`host/prng.rs` — `Prng` wraps `ChaCha20Rng` with metered byte-drawing. Base PRNG seeds per-frame sub-PRNGs deterministically. Separate unmetered PRNGs exist for recording-auth nonces and test data.

### Tracing

`host/trace.rs` — `TraceHook`, `TraceEvent`, `TraceRecord`, `TraceState` for lifecycle observation. Events include `PushCtx`, `PopCtx`, `EnvCall`, `EnvRet`. Used for debugging and testing only; disabled during debug-mode operations to avoid observation leaks.

---

## Key Control Flows

### Contract Invocation
1. Embedder calls `e2e_invoke` with `HostFunction`, footprint, auth entries.
2. Host is constructed with `Storage` and `Budget` from network config.
3. `ModuleCache` is populated or provided externally.
4. `Host::with_frame(Frame::HostFunction, ..)` pushes top-level frame.
5. For `InvokeContract`: resolves contract instance, loads Wasm, instantiates `Vm`.
6. `Host::with_frame(Frame::ContractVM, ..)` pushes contract frame with rollback point.
7. VM calls exported function. Wasm instructions consume wasmi fuel.
8. Host functions called from Wasm go through dispatch: fuel→budget, args relative→absolute, call `VmCallerEnv` method, result absolute→relative, budget→fuel.
9. On return, frame pops. On error, storage/events roll back to rollback point.
10. `Host::try_finish()` extracts final `(Storage, Events)`.

### Object Lifecycle
1. Host code calls `add_host_object(value)` → pushes to `objects` vec, returns absolute handle.
2. When returning to Wasm, `absolute_to_relative` adds to per-frame relative table, returns relative handle.
3. When Wasm calls host, `relative_to_absolute` looks up in relative table, returns absolute handle.
4. `visit_obj(handle, closure)` indexes into objects vec, calls closure with typed reference.
5. Objects are immutable once created. No garbage collection; lifetime is the host's lifetime.

### Budget Flow
1. Budget initialized from network config cost params + CPU/mem limits.
2. Every host operation calls `Budget::charge(CostType, input)`.
3. `charge` computes `cost = const_term + lin_term * input`, adds to dimension total, checks limit.
4. Wasm execution uses wasmi fuel; fuel is derived from remaining CPU budget and transferred at host function boundaries.
5. Shadow mode (debug/diagnostic work) tracks but doesn't enforce limits.
