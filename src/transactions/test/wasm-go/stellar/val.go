package stellar

import "math/bits"

const valObjVoid uint64 = 0
const valObjBoolTrue uint64 = 1
const valObjBoolFalse uint64 = 2

type tag uint16

const (
	tagObject tag = 0
	tagU32    tag = 1
	tagI32    tag = 2
	tagSymbol tag = 3
	tagBitset tag = 4
	tagTimePt tag = 5
	tagStatus tag = 6
)

type Error uint16

const (
	ErrorNone          Error = 0
	ErrorValInvalidTag Error = 0b1000000000000000 | iota
	ErrorValInvalidBody
	ErrorValWrongType
	ErrorValBodyMalformed
)

type Val uint64

func valFromTagBody(t tag, body uint64) (Val, Error) {
	if t >= 8 {
		return 0, ErrorValInvalidTag
	}
	body = bits.RotateLeft64(body, 16)
	if body&0xffff != 0 {
		return 0, ErrorValInvalidBody
	}
	return Val(body | uint64(t)), ErrorNone
}

func mustVal(v Val, e Error) Val {
	if e != ErrorNone {
		panic(e)
	}
	return v
}

func (v Val) tag() tag {
	return tag(v)
}

func (v Val) body() uint64 {
	return uint64(v) >> 16
}

func (v Val) Log() {
	logValue(v)
}

func StatusVal(s uint32) Val {
	return mustVal(valFromTagBody(tagStatus, uint64(s)))
}

func Int32Val(u int32) Val {
	return mustVal(valFromTagBody(tagI32, uint64(u)))
}

func (v Val) IsInt32() bool {
	return v.tag() == tagI32
}

func (v Val) Int32() (int32, Error) {
	if v.tag() != tagI32 {
		return 0, ErrorValWrongType
	}
	body := v.body()
	u := int32(body)
	if uint64(u) != body {
		return 0, ErrorValBodyMalformed
	}
	return u, ErrorNone
}

func Uint32Val(u uint32) Val {
	return mustVal(valFromTagBody(tagU32, uint64(u)))
}

func (v Val) IsUint32() bool {
	return v.tag() == tagU32
}

func (v Val) Uint32() (uint32, Error) {
	if v.tag() != tagU32 {
		return 0, ErrorValWrongType
	}
	body := v.body()
	u := uint32(body)
	if uint64(u) != body {
		return 0, ErrorValBodyMalformed
	}
	return u, ErrorNone
}
