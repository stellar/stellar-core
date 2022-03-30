#![no_std]
use stellar_contract_sdk as sdk;
use sdk::Val;

#[no_mangle]
pub fn pay(src: Val, dst: Val, asset: Val, amount: Val) -> Val {
    sdk::pay(src, dst, asset, amount)
}
