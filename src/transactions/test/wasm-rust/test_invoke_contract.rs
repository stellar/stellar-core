#![allow(dead_code)]

extern {
    fn log_value(v:i64) -> i64;
    fn map_new() -> i64;
    fn map_put(m:i64,k:i64,v:i64) -> i64;
    fn map_get(m:i64,k:i64) -> i64;
    fn get_current_ledger_num() -> i64;
}

#[no_mangle]
pub unsafe fn invoke(arg: i64) -> i64 {
    let _ = log_value(arg);
    let _ = log_value(get_current_ledger_num());
    return arg + 1;
}
