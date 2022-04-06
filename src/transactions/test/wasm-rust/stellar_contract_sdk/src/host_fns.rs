use super::{Val,Object};

// Most host functions have strong contractual guarantees from the host: they
// will either return the correctly-typed objects here, or they will trap. So we
// do not need to check return types, and we can even unsafely downcast Vals to
// specific subtypes if we know them.
//
// (Recall that there must be no way for the guest to corrupt the host even if
// the guest does receive unexpected objects -- at worst the host can corrupt or
// confuse the guest this way, but the guest can never defend itself against a
// malicious host anyways.)
//
// We do this mostly to minimize codesize, and also reduce the chance of users
// missing an error in a contract binding to another language that doesn't have
// Result<> types. Every error that can trap this way typically has a way to
// avoid it by checking some value first (eg. check a map to see if it contains
// a value before getting it).
//
// The exceptions are ledger-interaction calls and especially cross-contract
// calls: these we project into a Result<> because they are fairly failure-prone
// and impossible to guard against failure of, typically. We assume users might
// wish to contain these and, in general, that users won't be doing a _ton_ of
// them so it's ok that they are a little more expensive code-size-wise.

extern "C" {
    pub(crate) fn host__log_value(v: Val) -> Val;

    pub(crate) fn host__map_new() -> Object;
    pub(crate) fn host__map_put(m: Object, k: Val, v: Val) -> Object;
    pub(crate) fn host__map_get(m: Object, k: Val) -> Val;
    pub(crate) fn host__map_del(m: Object, k: Val) -> Object;
    pub(crate) fn host__map_len(m: Object) -> Val;
    pub(crate) fn host__map_keys(m: Object) -> Object;

    pub(crate) fn host__vec_new() -> Object;
    pub(crate) fn host__vec_put(v: Object, i: Val, x: Val) -> Object;
    pub(crate) fn host__vec_get(v: Object, i: Val) -> Val;
    pub(crate) fn host__vec_del(v: Object, i: Val) -> Object;
    pub(crate) fn host__vec_len(v: Object) -> Val;

    pub(crate) fn host__vec_push(v: Object, x: Val) -> Object;
    pub(crate) fn host__vec_pop(v: Object) -> Object;
    pub(crate) fn host__vec_take(v: Object, n: Val) -> Object;
    pub(crate) fn host__vec_drop(v: Object, n: Val) -> Object;
    pub(crate) fn host__vec_front(v: Object) -> Val;
    pub(crate) fn host__vec_back(v: Object) -> Val;
    pub(crate) fn host__vec_insert(v: Object, i: Val, x: Val) -> Object;
    pub(crate) fn host__vec_append(v1: Object, v2: Object) -> Object;

    pub(crate) fn host__get_current_ledger_num() -> Val;

    // NB: this returns a raw/unboxed u64, not a Val union.
    pub(crate) fn host__get_current_ledger_close_time() -> u64;

    // NB: returns a Status; details can be fetched with
    // get_last_operation_result.
    pub(crate) fn host__pay(src: Val, dst: Val, asset: Val, amount: Val) -> Val;

    // NB: returns callee-return-value-or-Status; details can be fetched with
    // get_last_operation_result.
    //
    // TODO: possibly revisit this since it adds ambiguity to whether callee
    // successfully returned a status, or call failed and failure _generated_ a
    // status. Possibly this distinction is too fussy to disambiguate.
    pub(crate) fn host__call0(contract: Val, func: Val) -> Val;
    pub(crate) fn host__call1(contract: Val, func: Val, a: Val) -> Val;
    pub(crate) fn host__call2(contract: Val, func: Val, a: Val, b: Val) -> Val;
    pub(crate) fn host__call3(contract: Val, func: Val, a: Val, b: Val, c: Val) -> Val;
    pub(crate) fn host__call4(contract: Val, func: Val, a: Val, b: Val, c: Val, d: Val) -> Val;

    // Fetches an OpResult object for inspection, in the rare case the user
    // wants more detail than is conveyed in a simple Status.
    pub(crate) fn host__get_last_operation_result() -> Object;

    pub(crate) fn host__bignum_from_u64(x: u64) -> Object;
    pub(crate) fn host__bignum_add(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_sub(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_mul(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_div(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_rem(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_and(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_or(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_xor(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_shl(lhs: Object, rhs: u64) -> Object;
    pub(crate) fn host__bignum_shr(lhs: Object, rhs: u64) -> Object;
    pub(crate) fn host__bignum_cmp(lhs: Object, rhs: Object) -> Val;
    pub(crate) fn host__bignum_is_zero(x: Object) -> Val;
    pub(crate) fn host__bignum_neg(x: Object) -> Object;
    pub(crate) fn host__bignum_not(x: Object) -> Object;
    pub(crate) fn host__bignum_gcd(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_lcm(lhs: Object, rhs: Object) -> Object;
    pub(crate) fn host__bignum_pow(lhs: Object, rhs: u64) -> Object;
    pub(crate) fn host__bignum_pow_mod(p: Object, q: Object, m: Object) -> Object;
    pub(crate) fn host__bignum_sqrt(x: Object) -> Object;
}
