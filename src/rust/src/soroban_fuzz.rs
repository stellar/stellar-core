// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Soroban fuzz target integration for stellar-core.
//!
//! This module provides a bridge to the Soroban fuzz targets defined in the
//! soroban-env-host crate. The actual fuzz target implementations live in
//! soroban_env_host::fuzz; this module just provides the C++ bridge interface.
//!
//! Note: The fuzz module in soroban-env-host is available when the `testutils`
//! feature is enabled. This allows fuzz smoke tests to run in normal BUILD_TESTS
//! builds without requiring the full fuzz instrumentation infrastructure.

use crate::rust_bridge::FuzzResultCode;

// Stub implementation when testutils feature is disabled
#[cfg(not(all(feature="next", feature = "testutils")))]
pub fn run_soroban_fuzz_target(_name: &str, _data: &[u8]) -> FuzzResultCode {
    FuzzResultCode::FUZZ_DISABLED
}

// When testutils feature is enabled, import and use the actual implementations
#[cfg(all(feature="next", feature = "testutils"))]
extern crate soroban_env_host_p26;

#[cfg(all(feature="next", feature = "testutils"))]
use soroban_env_host_p26::fuzz::{self, FuzzResult};

/// Run a Soroban fuzz target with the given input bytes.
/// Panics on internal errors (which is what the fuzzer wants to find!)
#[cfg(all(feature="next", feature = "testutils"))]
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
