---
name: subsystem-summary-of-rust
description: "read this skill for a token-efficient summary of the rust subsystem"
---

# Rust Subsystem (Non-Soroban) — Technical Summary

## Overview

The rust subsystem provides a Rust static library (`rust_stellar_core`) that is linked into the stellar-core C++ binary. It uses the `cxx` crate (v1.0.97) to define a bidirectional FFI bridge between C++ and Rust. The subsystem's primary responsibilities are:

1. **Soroban host function invocation** — dispatching to the correct protocol-versioned soroban host.
2. **Fee computation** — transaction resource fees, rent fees, and rent write fees.
3. **Module caching** — pre-compiled WASM module cache for Soroban contracts.
4. **128-bit integer arithmetic** — exposing Rust's native `i128` to C++.
5. **Base64 encoding/decoding** — used for XDR serialization interop.
6. **Ed25519 signature verification** — using `ed25519-dalek` for faster verification.
7. **Logging bridge** — routing Rust `log` crate output to the C++ spdlog system.
8. **Quorum intersection checking** — using the `stellar-quorum-analyzer` SAT solver.
9. **Utility functions** — rustc version, executable path, backtrace capture, XDR version checks.

The crate is built as `crate-type = ["staticlib"]` (edition 2021, rust-version 1.82.0). Optional features include `tracy` (profiling), `next` (pre-release protocol), `testutils` (test-only code), and `unified` (IDE-friendly single cargo build).

## File Layout

| File | Role |
|------|------|
| `Cargo.toml` | Crate metadata, multi-host soroban dependencies, feature flags |
| `src/lib.rs` | Crate root; declares modules, re-exports bridge symbols, defines `tracy_span!` macro |
| `src/bridge.rs` | `#[cxx::bridge]` module — all FFI type/function declarations |
| `src/common.rs` | `RustBuf`/`CxxBuf`/`BridgeError` impls; `get_rustc_version`, `current_exe`, `capture_cxx_backtrace`, `check_xdr_version_identities` |
| `src/b64.rs` | `to_base64` / `from_base64` |
| `src/ed25519_verify.rs` | `verify_ed25519_signature_dalek` (unsafe raw-pointer FFI) |
| `src/i128.rs` | `i128_add`, `i128_sub`, overflow/underflow checks, conversion |
| `src/log.rs` | `StellarLogger` implementing `log::Log`, routes to C++ spdlog |
| `src/quorum_checker.rs` | `network_enjoys_quorum_intersection` wrapping `stellar-quorum-analyzer` |
| `src/soroban_invoke.rs` | `invoke_host_function`, fee computation, transaction parsing dispatchers |
| `src/soroban_module_cache.rs` | `SorobanModuleCache` struct; per-protocol caches |
| `src/soroban_proto_all.rs` | Protocol-versioned host modules (p21–p26), dispatch table, adaptors |
| `src/soroban_proto_any.rs` | Protocol-agnostic host invocation code, mounted inside each pN module |
| `CppShims.h` | Thin C++ shim functions (`shim_isLogLevelAtLeast`, `shim_logAtPartitionAndLevel`) |
| `RustBridge.h` | cxx-generated C++ header with all bridge types and function declarations |
| `RustBridge.cpp` | cxx-generated C++ implementation (extern "C" thunks, Vec/Box specializations) |
| `RustVecXdrMarshal.h` | Declares `rust::Vec<uint8_t>` as valid xdrpp byte buffer type |

## The CXX Bridge Mechanism

### How it works

The bridge is defined in `src/bridge.rs` inside a `#[cxx::bridge]` attribute macro on `mod rust_bridge`. This module contains three sections:

1. **Shared types** — structs and enums visible to both sides, defined once:
   - `CxxBuf` (C++→Rust data: wraps `UniquePtr<CxxVector<u8>>`)
   - `RustBuf` (Rust→C++ data: wraps `Vec<u8>`)
   - `XDRFileHash`, `InvokeHostFunctionOutput`, `CxxLedgerInfo`, `CxxTransactionResources`, `CxxFeeConfiguration`, `CxxLedgerEntryRentChange`, `CxxRentFeeConfiguration`, `CxxRentWriteFeeConfiguration`, `CxxI128`, `FeePair`, `SorobanVersionInfo`
   - Enums: `LogLevel` (shared with `stellar::LogLevel`), `BridgeError`, `QuorumCheckerStatus`
   - `QuorumSplit`, `QuorumCheckerResource`

2. **`extern "Rust"` block** (`#[namespace = "stellar::rust_bridge"]`) — Rust functions callable from C++:
   - All functions listed in the "Key Functions" section below.
   - The opaque type `SorobanModuleCache` with its methods.

3. **`extern "C++"` block** (`#[namespace = "stellar"]`) — C++ functions callable from Rust:
   - `shim_isLogLevelAtLeast(partition: &CxxString, level: LogLevel) -> Result<bool>`
   - `shim_logAtPartitionAndLevel(partition: &CxxString, level: LogLevel, msg: &CxxString) -> Result<()>`

### Data passing convention

- **C++ → Rust**: Data is passed as `CxxBuf` containing `UniquePtr<CxxVector<u8>>` (a C++-allocated `std::vector<uint8_t>`). The Rust side reads from it via `data.as_slice()`.
- **Rust → C++**: Data is returned as `RustBuf` containing `Vec<u8>` (Rust-allocated). The C++ side reads from `data` (a `rust::Vec<uint8_t>`).
- XDR serialization/deserialization is done with `ReadXdr`/`WriteXdr` using `non_metered_xdr_from_cxx_buf` and `non_metered_xdr_to_rust_buf` helper functions with a depth limit of 1000 and length limit matching the buffer size.
- `RustVecXdrMarshal.h` allows xdrpp to directly unmarshal from `rust::Vec<uint8_t>`.

### Generated files

`RustBridge.h` and `RustBridge.cpp` are generated by the `cxxbridge` tool. They contain:
- Full implementations of `rust::String`, `rust::Slice<T>`, `rust::Box<T>`, `rust::Vec<T>`, `rust::Opaque`, `rust::Error`.
- C struct definitions mirroring the shared types.
- `static_assert` checks ensuring `LogLevel` enum values match between C++ and Rust.
- `extern "C"` function declarations for the mangled bridge symbols.
- C++ wrapper functions in `namespace stellar::rust_bridge` that call through extern "C" thunks and translate Rust errors to C++ exceptions (`rust::Error`).
- Template specializations for `rust::Vec<RustBuf>`, `rust::Vec<XDRFileHash>`, `rust::Vec<CxxBuf>`, etc.
- `rust::Box<SorobanModuleCache>` alloc/dealloc/drop specializations.

### CppShims.h

Provides simple inline wrapper functions that cxx.rs can call, bridging to C++ APIs that are too complex for cxx to handle directly (e.g., static member functions):
- `shim_isLogLevelAtLeast` → `Logging::isLogLevelAtLeast`
- `shim_logAtPartitionAndLevel` → `Logging::logAtPartitionAndLevel`

## Key Data Structures

### `CxxBuf` / `RustBuf`
Directional byte-buffer wrappers for passing XDR-serialized data across the FFI boundary. `CxxBuf` owns a `std::unique_ptr<std::vector<uint8_t>>` (C++ allocated). `RustBuf` owns a `Vec<u8>` (Rust allocated). Both implement `AsRef<[u8]>`.

### `CxxI128`
Split representation of 128-bit integer: `{ hi: i64, lo: u64 }`. Used because C++ lacks native `i128` on all platforms. Converted to/from Rust `i128` via `int128_helpers::{i128_from_pieces, i128_hi, i128_lo}`.

### `InvokeHostFunctionOutput`
Return value of `invoke_host_function`. Contains:
- `success: bool`, `is_internal_error: bool`
- `diagnostic_events: Vec<RustBuf>` (XDR-encoded `DiagnosticEvent`)
- `cpu_insns`, `mem_bytes`, `time_nsecs` (and excluding-VM-instantiation variants)
- `result_value: RustBuf`, `contract_events: Vec<RustBuf>`, `modified_ledger_entries: Vec<RustBuf>`, `rent_fee: i64`

### `SorobanModuleCache`
An opaque Rust type exposed to C++ via `rust::Box<SorobanModuleCache>`. Holds per-protocol `ProtocolSpecificModuleCache` instances (p23, p24, p25, and optionally p26 with `next` feature). Each `ProtocolSpecificModuleCache` contains a `ModuleCache` (from soroban-env-host, threadsafe via internal locking) and an `AtomicU64` tracking memory consumption. Methods:
- `compile(&mut self, ledger_protocol: u32, wasm: &[u8])` — parse and cache a WASM module for the given protocol.
- `shallow_clone(&self) -> Box<SorobanModuleCache>` — clone shared ownership handles for multithreaded compilation.
- `evict_contract_code(&mut self, key: &[u8])` — remove a module from all protocol caches by 32-byte hash.
- `clear(&mut self)` — clear all protocol caches.
- `contains_module(&self, protocol: u32, key: &[u8]) -> bool`
- `get_mem_bytes_consumed(&self, protocol: u32) -> u64`

### `HostModule`
A dispatch table struct (not crossing FFI) containing function pointers for a specific protocol version's soroban host. Fields include `max_proto`, `invoke_host_function`, `compute_transaction_resource_fee`, `compute_rent_fee`, `compute_rent_write_fee_per_1kb`, `contract_code_memory_size_for_rent`, `can_parse_transaction`, and `get_soroban_version_info`. The static array `HOST_MODULES` holds one entry per protocol version (p21–p25/p26), populated via the `proto_versioned_functions_for_module!` macro.

### `ProtocolSpecificModuleCache`
Per-protocol cache wrapper (defined in `soroban_proto_any.rs`). Wraps a `ModuleCache` from the protocol's soroban-env-host and a `CoreCompilationContext` (unlimited budget for compilation). Supports `compile`, `evict`, `clear`, `contains_module`, `get_mem_bytes_consumed`, and `shallow_clone`.

### `CoreCompilationContext`
Implements `CompilationContext` (= `ErrorHandler + AsBudget`) with an unlimited budget, used for compiling WASM modules outside of transaction execution.

## Key Functions (Exported Rust → C++)

### Soroban Host Invocation
- `invoke_host_function(config_max_protocol: u32, enable_diagnostics: bool, instruction_limit: u32, hf_buf: &CxxBuf, resources: CxxBuf, restored_rw_entry_indices: &Vec<u32>, source_account: &CxxBuf, auth_entries: &Vec<CxxBuf>, ledger_info: CxxLedgerInfo, ledger_entries: &Vec<CxxBuf>, ttl_entries: &Vec<CxxBuf>, base_prng_seed: &CxxBuf, rent_fee_configuration: CxxRentFeeConfiguration, module_cache: &SorobanModuleCache) -> Result<InvokeHostFunctionOutput>` — Dispatches to the correct protocol-versioned host via `get_host_module_for_protocol`. Wraps the call in `panic::catch_unwind`.

### Fee Computation
- `compute_transaction_resource_fee(config_max_protocol: u32, protocol_version: u32, tx_resources: CxxTransactionResources, fee_config: CxxFeeConfiguration) -> Result<FeePair>` — Returns `(non_refundable_fee, refundable_fee)`.
- `compute_rent_fee(config_max_protocol: u32, protocol_version: u32, changed_entries: &Vec<CxxLedgerEntryRentChange>, fee_config: CxxRentFeeConfiguration, current_ledger_seq: u32) -> Result<i64>`
- `compute_rent_write_fee_per_1kb(config_max_protocol: u32, protocol_version: u32, bucket_list_size: i64, fee_config: CxxRentWriteFeeConfiguration) -> Result<i64>`
- `contract_code_memory_size_for_rent(config_max_protocol: u32, protocol_version: u32, contract_code_entry: &CxxBuf, cpu_cost_params: &CxxBuf, mem_cost_params: &CxxBuf) -> Result<u32>` — Only valid for protocol ≥ 23.

### Transaction Parsing
- `can_parse_transaction(config_max_protocol: u32, protocol_version: u32, xdr: &CxxBuf, depth_limit: u32) -> Result<bool>` — Checks if a `TransactionEnvelope` XDR can be deserialized in the given protocol.

### 128-bit Integer Arithmetic
- `i128_add(lhs: &CxxI128, rhs: &CxxI128) -> Result<CxxI128>`
- `i128_sub(lhs: &CxxI128, rhs: &CxxI128) -> Result<CxxI128>`
- `i128_add_will_overflow(lhs: &CxxI128, rhs: &CxxI128) -> Result<bool>`
- `i128_sub_will_underflow(lhs: &CxxI128, rhs: &CxxI128) -> Result<bool>`
- `i128_from_i64(val: i64) -> Result<CxxI128>`
- `i128_is_negative(val: &CxxI128) -> Result<bool>`
- `i128_i64_eq(lhs: &CxxI128, rhs: i64) -> Result<bool>`

### Ed25519 Verification
- `verify_ed25519_signature_dalek(public_key_ptr: *const u8, signature_ptr: *const u8, message_ptr: *const u8, message_len: usize) -> bool` — Unsafe raw-pointer interface. Uses `ed25519-dalek`'s `verify_strict` (rejects small-order points, matching libsodium). Never panics; returns false for invalid input.

### Base64
- `to_base64(b: &CxxVector<u8>, s: Pin<&mut CxxString>)` — Encode bytes to base64.
- `from_base64(s: &CxxString, b: Pin<&mut CxxVector<u8>>)` — Decode base64 with error-tolerant stripping of invalid characters.

### Logging
- `init_logging(maxLevel: LogLevel) -> Result<()>` — Initializes the `StellarLogger` as the global Rust logger, routing to C++ spdlog. Uses `AtomicBool` for one-time initialization. Log partitions (e.g., `TX`, `Ledger`, `SCP`) are defined in `log::partition` and must match `util/LogPartitions.def` on the C++ side.

### Quorum Checker
- `network_enjoys_quorum_intersection(nodes: &Vec<CxxBuf>, quorum_set: &Vec<CxxBuf>, potential_split: &mut QuorumSplit, resource_limit: &QuorumCheckerResource, resource_usage: &mut QuorumCheckerResource) -> Result<QuorumCheckerStatus>` — Returns `UNSAT` (quorum intersection holds), `SAT` (split found, populates `potential_split`), or `UNKNOWN`. Time limit enforced internally; memory limit is a hard abort via global allocator.

### Module Cache
- `new_module_cache() -> Result<Box<SorobanModuleCache>>`
- Methods on `SorobanModuleCache`: `compile`, `shallow_clone`, `evict_contract_code`, `clear`, `contains_module`, `get_mem_bytes_consumed`.

### Utility
- `get_rustc_version() -> String`
- `current_exe() -> Result<String>`
- `capture_cxx_backtrace() -> String` — Uses `backtrace` crate; filters out initial Rust frames and libc frames.
- `get_soroban_version_info(core_max_proto: u32) -> Vec<SorobanVersionInfo>` — Returns version info for all linked soroban hosts. Panics if no host supports the given protocol.
- `check_sensible_soroban_config_for_protocol(core_max_proto: u32)` — Validates HOST_MODULES are in ascending order and cover the max protocol.
- `check_xdr_version_identities() -> Result<()>` — Compares XDR file SHA256 hashes across crates.

## Multi-Protocol Soroban Host Architecture

### Design

stellar-core links multiple versions of `soroban-env-host` simultaneously, one per protocol version range. Each is labeled by its maximum supported protocol (e.g., `soroban-env-host-p21` supports protocols up to 21). At runtime, `get_host_module_for_protocol(config_max_proto, ledger_protocol)` selects the appropriate host.

### Implementation pattern

`soroban_proto_all.rs` defines adaptor modules `p21`, `p22`, `p23`, `p24`, `p25`, and conditionally `p26` (behind `next` feature). Each adaptor:
1. Imports its specific `soroban_env_host_pNN` crate and re-exports it as `soroban_env_host`.
2. Provides adapter functions for API differences between host versions (e.g., different field names in `TransactionResources`, `RentFeeConfiguration`).
3. Mounts `soroban_proto_any.rs` as a child module — this file is the same source but "sees" a different `super::soroban_env_host` in each context.
4. Defines stub types (`ModuleCache`, `ErrorHandler`, `CompilationContext`) for older protocols (p21, p22) that don't support the reusable module cache API.

### Protocol dispatch

The `HOST_MODULES` static array maps protocol ranges to `HostModule` structs containing function pointers. `get_host_module_for_protocol` iterates this array: each entry's implied minimum protocol is one more than the previous entry's `max_proto` (first entry starts at 0).

### Aliases

- `soroban_curr` — alias for the latest non-next host (p25, or p26 with `next`).
- `protocol_agnostic` — re-exports from p24 that are stable across versions (e.g., `int128_helpers`, `make_error`).

## Key Data Flows

### C++ → Soroban Invocation → C++
1. C++ constructs `CxxBuf` objects containing XDR-serialized data (host function, resources, ledger entries, etc.) and a `CxxLedgerInfo`.
2. Calls `stellar::rust_bridge::invoke_host_function(...)` which crosses the FFI boundary.
3. Rust dispatches to the correct `HostModule` based on `(config_max_protocol, ledger_info.protocol_version)`.
4. The protocol-specific `invoke_host_function` in `soroban_proto_any.rs` deserializes XDR, creates a `Budget`, optional trace hook, and calls through to `soroban_env_host::e2e_invoke::invoke_host_function`.
5. Results are re-serialized to `RustBuf` vectors and returned as `InvokeHostFunctionOutput`.
6. The C++ wrapper in `RustBridge.cpp` unwraps the result or throws `rust::Error` on failure.

### Logging (Rust → C++)
1. Rust code calls `log::info!()` etc.
2. `StellarLogger::log()` converts the level and calls `shim_logAtPartitionAndLevel` via the extern "C++" bridge.
3. The shim calls `Logging::logAtPartitionAndLevel` in C++.

### Module Cache Lifecycle
1. C++ calls `new_module_cache()` to get a `rust::Box<SorobanModuleCache>`.
2. Calls `compile(protocol, wasm_bytes)` to cache WASM modules (typically on startup and during catchup).
3. The cache is passed by reference to `invoke_host_function`.
4. `shallow_clone()` creates shared-ownership handles for multithreaded use.
5. `evict_contract_code(key)` removes entries; `clear()` empties all caches.

## Error Handling

- All fallible Rust functions return `Result<T, Box<dyn std::error::Error>>` (or `Result<T, HostError>`).
- cxx converts Rust `Err` returns into C++ `rust::Error` exceptions.
- `invoke_host_function` and `network_enjoys_quorum_intersection` additionally wrap their core logic in `panic::catch_unwind` to convert Rust panics into errors rather than unwinding across the FFI boundary.
- The quorum checker's memory limit is a hard abort (non-catchable) by design.
- `CoreHostError` enum wraps either a `HostError` from soroban or a general `String` message.

## Dependencies

| Crate | Purpose |
|-------|---------|
| `cxx` 1.0.97 | C++/Rust FFI bridge framework |
| `base64` 0.13.1 | Base64 encode/decode |
| `log` 0.4.19 | Rust logging facade |
| `ed25519-dalek` 2.1.1 | Ed25519 signature verification |
| `itertools` 0.10.5 | Iterator utilities |
| `backtrace` 0.3.76 | C++ backtrace capture (with `cpp_demangle`) |
| `rand` 0.8.5 | RNG (must match soroban's version) |
| `rustc-simple-version` 0.1.0 | Compile-time rustc version string |
| `tracy-client` 0.17.0 | Tracy profiling (optional) |
| `stellar-quorum-analyzer` | SAT-based quorum intersection checking |
| `soroban-env-host-pNN` | Protocol-specific Soroban hosts (p21–p26) |
| `soroban-test-wasms` | Pre-compiled test WASM binaries |
| `soroban-synth-wasm` | Random WASM generation for testing |

## Build Notes

- The default build does **not** use the optional `soroban-env-host-pNN` deps from Cargo.toml. Instead, each host is built as a separate cargo invocation and linked in (see `src/Makefile.am`). This avoids Cargo's dependency unification.
- The `unified` feature enables all hosts as direct dependencies for IDE usage. This perturbs `Cargo.lock` — changes should not be committed.
- Tracy feature flags must match between the Rust crate and the C++ `lib/tracy` submodule version.
