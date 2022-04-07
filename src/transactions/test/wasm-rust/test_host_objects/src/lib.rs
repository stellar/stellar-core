#![no_std]
use sdk::{log_value, Map, OrAbort, Symbol, Val};
use stellar_contract_sdk as sdk;

const STEP1: Symbol = Symbol::from_str("step1");
const STEP2: Symbol = Symbol::from_str("step2");
const STEP3: Symbol = Symbol::from_str("step3");
const STEP4: Symbol = Symbol::from_str("step4");

const KEY: Symbol = Symbol::from_str("key");

#[no_mangle]
pub fn invoke(k: Val, v: Val) -> Val {
    let k: Symbol = k.try_into().or_abort();

    sdk::put_contract_data(KEY.into(), v);
    sdk::put_contract_data(KEY.into(), v);
    sdk::put_contract_data(KEY.into(), v);
    log_value(sdk::has_contract_data(KEY.into()).into());

    log_value(STEP1.into());

    log_value(v);
    let v: u32 = v.try_into().or_abort();
    log_value(STEP2.into());

    let m: Map<Symbol, u32> = Map::new();
    log_value(STEP3.into());

    let m = m.put(k, v);
    log_value(STEP4.into());

    log_value(KEY.into());
    log_value(sdk::get_contract_data(KEY.into()));

    let r: u32 = m.get(k);
    (r + r).into()
}
