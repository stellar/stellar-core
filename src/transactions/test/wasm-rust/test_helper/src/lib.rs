#![no_std]
use stellar_contract_sdk as sdk;
use sdk::{Val,Symbol,log_value};

const IN_HELPER: Symbol = Symbol::from_str("in_helper");

#[no_mangle]
pub fn helper(x: Val) -> Val {
    log_value(IN_HELPER.into());
    log_value(x);
    (x.as_u32() + 13).into()
}
