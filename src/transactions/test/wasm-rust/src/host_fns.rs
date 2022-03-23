use super::Val;
extern "C" {
    pub(crate) fn log_value(v: Val) -> Val;
    pub(crate) fn map_new() -> Val;
    pub(crate) fn map_put(m: Val, k: Val, v: Val) -> Val;
    pub(crate) fn map_get(m: Val, k: Val) -> Val;
    pub(crate) fn get_current_ledger_num() -> Val;
}
