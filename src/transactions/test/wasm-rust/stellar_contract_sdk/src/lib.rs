#![no_std]
#![allow(dead_code)]

// This requires running on unstable.
//#![feature(alloc)]
//mod alloc;

mod bignum;
mod bitset;
mod host_fns;
mod map;
mod object;
mod or_abort;
mod result;
mod rt;
mod status;
mod vec;

mod symbol;
mod val;

pub use bignum::BigNum;
pub use bitset::BitSet;
pub use map::Map;
use object::ObjType;
pub use object::Object;
pub use or_abort::OrAbort;
pub use result::OpResult;
pub use status::Status;
pub use symbol::Symbol;
pub use val::Val;
pub use vec::Vec;

#[inline(always)]
pub fn require(b: bool) {
    b.or_abort();
}

#[inline(always)]
pub fn log_value(v: Val) {
    unsafe {
        host_fns::log_value(v);
    }
}

#[inline(always)]
pub fn get_current_ledger_num() -> u32 {
    unsafe { host_fns::get_current_ledger_num().as_u32_unchecked() }
}

#[inline(always)]
pub fn pay(src: Val, dst: Val, asset: Val, amount: Val) -> Val {
    unsafe { host_fns::pay(src, dst, asset, amount) }
}

#[inline(always)]
pub fn call0(contract: Val, func: Symbol) -> Val {
    unsafe { host_fns::call0(contract, func.into()) }
}

#[inline(always)]
pub fn call1(contract: Val, func: Symbol, a: Val) -> Val {
    unsafe { host_fns::call1(contract, func.into(), a) }
}

#[inline(always)]
pub fn call2(contract: Val, func: Symbol, a: Val, b: Val) -> Val {
    unsafe { host_fns::call2(contract, func.into(), a, b) }
}

#[inline(always)]
pub fn call3(contract: Val, func: Symbol, a: Val, b: Val, c: Val) -> Val {
    unsafe { host_fns::call3(contract, func.into(), a, b, c) }
}

#[inline(always)]
pub fn call4(contract: Val, func: Symbol, a: Val, b: Val, c: Val, d: Val) -> Val {
    unsafe { host_fns::call4(contract, func.into(), a, b, c, d) }
}

#[inline(always)]
pub fn get_last_operation_result() -> OpResult {
    unsafe { OpResult::unchecked_from_obj(host_fns::get_last_operation_result()) }
}
