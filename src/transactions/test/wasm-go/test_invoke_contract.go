package main

//export log_value
func logValue(int64) int64

//export map_new
func mapNew() int64

//export map_put
func mapPut(int64, int64, int64) int64

//export map_get
func mapGet(int64, int64) int64

//export get_current_ledger_num
func getCurrentLedgerNum() int64

//export get_current_ledger_close_time
func getCurrentLedgerCloseTime() int64

func main() {}

//export invoke
func invoke(arg int64) int64 {
	logValue(arg)
	logValue(getCurrentLedgerNum())
	logValue(getCurrentLedgerCloseTime())
	return arg + 1
}
