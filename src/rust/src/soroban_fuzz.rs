// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Soroban fuzz target integration for stellar-core.
//!
//! This module provides a bridge to the Soroban fuzz targets defined in the
//! soroban-fuzz-targets crate.
//!
//! Note: soroban-fuzz-targets imports _its own copy_ of soroban-env-host with
//! the `testutils` feature enabled. This allows fuzz smoke tests to run in
//! normal BUILD_TESTS builds without requiring the full fuzz instrumentation
//! infrastructure or even turning on `testutils` in the "production"
//! soroban-env-host crates (the per-protocol .rlib files).
//!
//! As a consequence, you should update the referenced commit of rs-soroban-env
//! in Cargo.toml that pulls in soroban-fuzz-targets any time you update a
//! soroban-env-host dependency, to keep fuzzing "something current". The
//! "host that gets fuzzed" does not automatically update.

use crate::rust_bridge::FuzzResultCode;

// Stub implementation when testutils feature is disabled
#[cfg(not(feature = "testutils"))]
pub fn run_soroban_fuzz_target(_name: &str, _data: &[u8]) -> FuzzResultCode {
    FuzzResultCode::FUZZ_DISABLED
}

// When testutils feature is enabled, import and use the actual implementations
#[cfg(feature = "testutils")]
use soroban_fuzz_targets::{self as fuzz, FuzzResult};

/// Run a Soroban fuzz target with the given input bytes.
/// Panics on internal errors (which is what the fuzzer wants to find!)
#[cfg(feature = "testutils")]
pub fn run_soroban_fuzz_target(name: &str, data: &[u8]) -> FuzzResultCode {
    let result = match name {
        "soroban_expr" => fuzz::expr::run_fuzz_target(data),
        "soroban_wasmi" => fuzz::wasmi::run_fuzz_target(data),
        _ => return FuzzResultCode::FUZZ_TARGET_UNKNOWN,
    };

    match result {
        FuzzResult::Ok => FuzzResultCode::FUZZ_SUCCESS,
        FuzzResult::Reject => FuzzResultCode::FUZZ_REJECTED,
        FuzzResult::InternalError => {
            panic!("fuzz target {name} had internal error");
        }
    }
}
