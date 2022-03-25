use super::Val;
extern "C" {
    pub(crate) fn log_value(v: Val) -> Val;

    pub(crate) fn map_new() -> Val;
    pub(crate) fn map_put(m: Val, k: Val, v: Val) -> Val;
    pub(crate) fn map_get(m: Val, k: Val) -> Val;
    pub(crate) fn map_del(m: Val, k: Val) -> Val;
    pub(crate) fn map_len(m: Val) -> Val;
    pub(crate) fn map_keys(m: Val) -> Val;

    pub(crate) fn vec_new() -> Val;
    pub(crate) fn vec_put(v: Val, i: Val, x: Val) -> Val;
    pub(crate) fn vec_get(v: Val, i: Val) -> Val;
    pub(crate) fn vec_del(v: Val, i: Val) -> Val;
    pub(crate) fn vec_len(v: Val) -> Val;

    pub(crate) fn vec_push(v: Val, x: Val) -> Val;
    pub(crate) fn vec_pop(v: Val) -> Val;
    pub(crate) fn vec_take(v: Val, n: Val) -> Val;
    pub(crate) fn vec_drop(v: Val, n: Val) -> Val;
    pub(crate) fn vec_front(v: Val) -> Val;
    pub(crate) fn vec_back(v: Val) -> Val;
    pub(crate) fn vec_insert(v: Val, i: Val, x: Val) -> Val;
    pub(crate) fn vec_append(v1: Val, v2: Val) -> Val;

    pub(crate) fn get_current_ledger_num() -> Val;
    pub(crate) fn get_current_ledger_close_time() -> Val;
}
