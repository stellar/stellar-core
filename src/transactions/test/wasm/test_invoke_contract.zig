extern fn log_value(i64) i64;
extern fn map_new() i64;
extern fn map_put(i64,i64,i64) i64;
extern fn map_get(i64,i64) i64;
extern fn get_current_ledger_num() i64;

export fn invoke(arg: i64) i64 {
    _ = log_value(arg);
    _ = log_value(get_current_ledger_num());
    return arg + 1;
}
