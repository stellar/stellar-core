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

// NB: this can't be called `pay` since it will collide with the extern fn `pay`
// we're _importing_ and LLVM will decide it's infinite recursion and optimize
// it to an infinite loop, which is pretty great.
#[no_mangle]
pub fn transfer(src: Val, dst: Val, asset: Val, amount: Val) -> Val {
    stellar::pay(src, dst, asset, amount)
}
