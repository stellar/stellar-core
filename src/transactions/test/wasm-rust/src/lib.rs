#![allow(dead_code)]

mod host_fns;
mod map;
mod or_abort;
mod symbol;
mod val;

pub use map::Map;
pub use or_abort::OrAbort;
pub use val::{BitSet, Object, Symbol, TimePt, Val};

#[inline(always)]
pub fn require(b: bool) {
    b.or_abort();
}

#[inline(always)]
pub fn log_value(v: Val) -> Val {
    unsafe { host_fns::log_value(v) }
}

#[inline(always)]
pub fn get_current_ledger_num() -> u32 {
    unsafe { host_fns::get_current_ledger_num().as_u32() }
}
