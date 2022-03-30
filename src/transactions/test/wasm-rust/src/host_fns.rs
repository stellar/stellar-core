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
}
