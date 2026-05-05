// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Soroban parallel-apply phase and canonical in-memory Soroban state, owned by
// Rust. Replaces the C++ InMemorySorobanState class and the C++ parallel-apply
// orchestration in LedgerManagerImpl / ParallelApplyUtils. The module is split
// into the same shape the legacy C++ code had:
//
//   * `state`        — the canonical in-memory Soroban state (SorobanState +
//                      typed CRUD; mirrors the old C++ InMemorySorobanState).
//   * `common`       — shared helpers used by both the state and the per-op
//                      drivers (layered_get, build_tx_delta, AccumulatedWrites,
//                      LedgerEntry/key utilities, prng / memo helpers, etc.).
//   * `invoke`       — InvokeHostFunction op driver (apply_invoke_host_function
//                      + the typed/bytes host-call wrappers and metric event
//                      construction).
//   * `extend`       — ExtendFootprintTtl op driver.
//   * `restore`      — RestoreFootprint op driver.
//   * `orchestrator` — apply_soroban_phase + run_cluster + dispatch_one_tx +
//                      apply_phase_writes_to_state. The ledger-close-time
//                      glue that walks the TxSet's stages/clusters and
//                      drives the per-op modules above.
//
// Everything outside this module sees the API through the cxx bridge in
// `bridge.rs`, which `use`s this module's public items.

mod common;
mod extend;
mod invoke;
mod orchestrator;
mod restore;
mod state;

#[cfg(test)]
mod tests;

// Public surface. Limited to the types and functions that the cxx bridge in
// `bridge.rs` (`use crate::soroban_apply::*`) needs to reach.
pub use orchestrator::apply_soroban_phase;
pub use state::{new_soroban_state, SorobanState};
