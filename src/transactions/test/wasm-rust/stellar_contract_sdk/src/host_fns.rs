use super::Val;
extern "C" {
    pub(crate) fn host__log_value(v: Val) -> Val;

    pub(crate) fn host__map_new() -> Val;
    pub(crate) fn host__map_put(m: Val, k: Val, v: Val) -> Val;
    pub(crate) fn host__map_get(m: Val, k: Val) -> Val;
    pub(crate) fn host__map_del(m: Val, k: Val) -> Val;
    pub(crate) fn host__map_len(m: Val) -> Val;
    pub(crate) fn host__map_keys(m: Val) -> Val;

    pub(crate) fn host__vec_new() -> Val;
    pub(crate) fn host__vec_put(v: Val, i: Val, x: Val) -> Val;
    pub(crate) fn host__vec_get(v: Val, i: Val) -> Val;
    pub(crate) fn host__vec_del(v: Val, i: Val) -> Val;
    pub(crate) fn host__vec_len(v: Val) -> Val;

    pub(crate) fn host__vec_push(v: Val, x: Val) -> Val;
    pub(crate) fn host__vec_pop(v: Val) -> Val;
    pub(crate) fn host__vec_take(v: Val, n: Val) -> Val;
    pub(crate) fn host__vec_drop(v: Val, n: Val) -> Val;
    pub(crate) fn host__vec_front(v: Val) -> Val;
    pub(crate) fn host__vec_back(v: Val) -> Val;
    pub(crate) fn host__vec_insert(v: Val, i: Val, x: Val) -> Val;
    pub(crate) fn host__vec_append(v1: Val, v2: Val) -> Val;

    pub(crate) fn host__get_current_ledger_num() -> Val;

    // NB: this returns a raw/unboxed u64, not a Val union.
    pub(crate) fn host__get_current_ledger_close_time() -> u64;

    pub(crate) fn host__pay(src: Val, dst: Val, asset: Val, amount: Val) -> Val;

    pub(crate) fn host__call0(contract: Val, func: Val) -> Val;
    pub(crate) fn host__call1(contract: Val, func: Val, a: Val) -> Val;
    pub(crate) fn host__call2(contract: Val, func: Val, a: Val, b: Val) -> Val;
    pub(crate) fn host__call3(contract: Val, func: Val, a: Val, b: Val, c: Val) -> Val;
    pub(crate) fn host__call4(contract: Val, func: Val, a: Val, b: Val, c: Val, d: Val) -> Val;

    pub(crate) fn host__bignum_from_u64(x: u64) -> Val;
    pub(crate) fn host__bignum_add(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_sub(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_mul(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_div(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_rem(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_and(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_or(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_xor(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_shl(lhs: Val, rhs: u64) -> Val;
    pub(crate) fn host__bignum_shr(lhs: Val, rhs: u64) -> Val;
    pub(crate) fn host__bignum_cmp(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_is_zero(x: Val) -> Val;
    pub(crate) fn host__bignum_neg(x: Val) -> Val;
    pub(crate) fn host__bignum_not(x: Val) -> Val;
    pub(crate) fn host__bignum_gcd(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_lcm(lhs: Val, rhs: Val) -> Val;
    pub(crate) fn host__bignum_pow(lhs: Val, rhs: u64) -> Val;
    pub(crate) fn host__bignum_pow_mod(p: Val, q: Val, m: Val) -> Val;
    pub(crate) fn host__bignum_sqrt(x: Val) -> Val;
}
