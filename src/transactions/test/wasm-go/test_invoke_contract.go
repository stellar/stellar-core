package main

import "invoke/stellar"

func main() {}

//export invoke
func invoke(arg stellar.Val) stellar.Val {
	i, err := arg.Int32()
	if err != stellar.ErrorNone {
		return stellar.StatusVal(uint32(err))
	}
	i /= 2
	i += 4
	return stellar.Int32Val(i)
}
