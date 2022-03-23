extern "C" {
    pub(crate) fn log_value(v: u64) -> u64;
    pub(crate) fn map_new() -> u64;
    pub(crate) fn map_put(m: u64, k: u64, v: u64) -> u64;
    pub(crate) fn map_get(m: u64, k: u64) -> u64;
    pub(crate) fn get_current_ledger_num() -> u64;
}
