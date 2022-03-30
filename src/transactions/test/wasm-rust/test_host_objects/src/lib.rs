#![no_std]
use stellar_contract_sdk as sdk;
use sdk::{Map,Val,Symbol,log_value};

const STEP1: Symbol = Symbol::from_str("step1");
const STEP2: Symbol = Symbol::from_str("step2");
const STEP3: Symbol = Symbol::from_str("step3");
const STEP4: Symbol = Symbol::from_str("step4");

#[no_mangle]
pub fn invoke(k: Val, v: Val) -> Val {

    let k: Symbol = k.as_symbol();
    log_value(STEP1.into());

    log_value(v);
    let v: u32 = v.as_u32();
    log_value(STEP2.into());
 
    let m: Map<Symbol,u32> = Map::new();
    log_value(STEP3.into());

    let m = m.put(k, v);
    log_value(STEP4.into());

    let r: u32 = m.get(k);
    (r + r).into()
}
