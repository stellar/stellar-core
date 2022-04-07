#![no_std]
use sdk::{BigNum, Map, OrAbort, Symbol, Val, Vec};
use stellar_contract_sdk as sdk;

#[no_mangle]
pub fn call_everything() -> Val {
    let n = 0;
    let v: Val = n.into();
    const S: Symbol = Symbol::from_str("S");
    let _ = sdk::call0(v, S);
    let _ = sdk::call1(v, S, v);
    let _ = sdk::call2(v, S, v, v);
    let _ = sdk::call3(v, S, v, v, v);
    let _ = sdk::call4(v, S, v, v, v, v);

    let _ = sdk::get_current_ledger_num();
    let _ = sdk::get_last_operation_result();
    let _ = sdk::log_value(v);

    let m: Map<u32, u32> = Map::new();
    let m = m.put(n, n);
    let n = m.get(n);
    let m = m.del(n);
    let _k = m.keys();

    let v: Vec<u32> = Vec::new();
    let v = v.put(n, n);
    let n = v.get(n);
    let v = v.del(n);
    let n = v.len();
    let v = v.push(n);
    let v = v.pop();
    let v = v.take(n);
    let v = v.drop(n);
    let _n = v.front();
    let _n = v.back();
    let v = v.insert(n, n);
    let _v = v.append(v);

    let bn: BigNum = 0.try_into().or_abort();
    let bn = bn + bn;
    let bn = bn - bn;
    let bn = bn * bn;
    let bn = bn / bn;
    let bn = bn % bn;
    let bn = bn & bn;
    let bn = bn | bn;
    let bn = bn ^ bn;
    let bn = bn >> 1;
    let bn = bn << 1;
    let _ord = bn.cmp(&bn);
    let _ = bn.is_zero();
    let bn = !bn;
    let bn = -bn;
    let bn = bn.gcd(bn);
    let bn = bn.lcm(bn);
    let bn = bn.pow(1);
    let bn = bn.pow_mod(bn, bn);
    let _bn = bn.sqrt();
    Val::from_void()
}
