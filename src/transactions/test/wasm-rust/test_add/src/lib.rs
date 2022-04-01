#![no_std]
use stellar_contract_sdk as sdk;
use sdk::{Val, OrAbort};

#[no_mangle]
pub fn add(a: Val, b: Val) -> Val {

    let a:i64 = a.try_into().or_abort();
    let b:i64 = b.try_into().or_abort();

    let c = a + b;

    return c.try_into().or_abort();
}
