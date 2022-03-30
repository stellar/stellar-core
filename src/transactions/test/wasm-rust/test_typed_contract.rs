#![no_std]

#[path="src/lib.rs"]
mod stellar;
use stellar::{Map,Val,Symbol};

const STEP1: Symbol = Symbol::from_str("step1");
const STEP2: Symbol = Symbol::from_str("step2");
const STEP3: Symbol = Symbol::from_str("step3");
const STEP4: Symbol = Symbol::from_str("step4");

#[no_mangle]
pub fn invoke(k: Val, v: Val) -> Val {

    let k: Symbol = k.as_symbol();
    stellar::log_value(STEP1.into());
    stellar::log_value(v);
    let v: u32 = v.as_u32();
    stellar::log_value(STEP2.into());
 
    let m: Map<Symbol,u32> = Map::new();
    stellar::log_value(STEP3.into());
    let m = m.put(k, v);
    stellar::log_value(STEP4.into());
    let r: u32 = m.get(k);

    (r + r).into()
}

#[no_mangle]
pub fn pay(src: Val, dst: Val, asset: Val, amount: Val) -> Val {
    stellar::pay(src, dst, asset, amount)
}

#[repr(transparent)]
pub struct OtherContract(Val);
impl OtherContract {
    #[inline(always)]
    pub fn other(&self, v:u32) -> Val {
        const OTHER: Symbol = Symbol::from_str("other");
        stellar::call1(self.0, OTHER, v.into())
    }
}

#[no_mangle]
pub fn call_other(contract: OtherContract) -> Val {
    contract.other(10)
}