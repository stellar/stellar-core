#![no_std]

#[path="src/lib.rs"]
mod stellar;
use stellar::{Map,Val,Symbol};

#[no_mangle]
pub fn invoke(k: Val, v: Val) -> Val {

    let k: Symbol = k.as_symbol();
    let v: u32 = v.as_u32();
 
    let m: Map<Symbol,u32> = Map::new();
    let m = m.put(k, v);
    let r: u32 = m.get(k);

    (r + r).into()
}
