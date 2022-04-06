#![no_std]
use sdk::{BigNum, OrAbort, Val};
use stellar_contract_sdk as sdk;

#[no_mangle]
pub fn bignum() -> Val {
    let a: BigNum = (10000).into();
    let a = a.pow(32);
    let b: BigNum = (10000).into();
    let b = b.pow(50);
    let c = ((a + b) * (a + b)).sqrt();
    return c.try_into().or_abort();
}
