#![no_std]
use sdk::{log_value, Symbol, Val};
use stellar_contract_sdk as sdk;

const IN_HELPER: Symbol = Symbol::from_str("in_helper");

#[no_mangle]
pub fn helper(x: Val) -> Val {
    log_value(IN_HELPER.into());
    log_value(x);
    (x.as_u32() + 13).into()
}
