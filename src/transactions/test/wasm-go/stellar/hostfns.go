package stellar

//export log_value
func logValue(Val) Val

//export map_new
func mapNew() Val

//export map_put
func mapPut(Val, Val, Val) Val

//export map_get
func mapGet(Val, Val) Val

//export get_current_ledger_num
func getCurrentLedgerNum() Val

//export get_current_ledger_close_time
func getCurrentLedgerCloseTime() Val
