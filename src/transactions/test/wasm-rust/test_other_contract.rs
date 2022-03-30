#![no_std]

#[path="src/lib.rs"]
mod stellar;
use stellar::{Val,Symbol};

const IN_OTHER: Symbol = Symbol::from_str("in_other");

#[no_mangle]
pub fn other(x: Val) -> Val {
    stellar::log_value(IN_OTHER.into());
    stellar::log_value(x);
    (x.as_u32() + 13).into()
}
