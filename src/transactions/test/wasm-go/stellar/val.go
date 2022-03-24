package stellar

const valBodyMask uint64 = 0x0000_ffff_ffff_ffff

const valStaticSubtagMask uint64 = 0x0000_ffff_0000_0000
const valStaticSubtagErr uint64 = 0x0000_ffff_0000_0000
const valStaticBodyVoid uint64 = 0
const valStaticBodyBoolTrue uint64 = 1
const valStaticBodyBoolFalse uint64 = 2

type tag uint8

const (
	tagStatic tag = 0
	tagU32    tag = 1
	tagI32    tag = 2
	tagSymbol tag = 3
	tagBitset tag = 4
	tagTimePt tag = 5
	tagObject tag = 6
)

type Error uint16

const (
	ErrorNone       Error = 0
	ErrorInvalidTag Error = 0b1000000000000000 | iota
	ErrorInvalidBody
	ErrorWrongType
	ErrorBodyMalformed
)

type Val uint64

func valFromTagBody(t tag, body uint64) (Val, Error) {
	if t >= 0x8 {
		return 0, ErrorInvalidTag
	}
	if body&valBodyMask != body {
		return 0, ErrorInvalidBody
	}
	return Val(uint64(t)<<48 | body), ErrorNone
}

func mustVal(v Val, e Error) Val {
	if e != ErrorNone {
		panic(e)
	}
	return v
}

func (v Val) raw() uint64 {
	return uint64(v)
}

func (v Val) tag() tag {
	return tag(v >> 48)
}

func (v Val) body() uint64 {
	return v.raw() & valBodyMask
}

func (v Val) Log() {
	logValue(v)
}

func ErrorVal(e Error) Val {
	body := valStaticSubtagErr | uint64(e)
	return mustVal(valFromTagBody(tagStatic, body))
}

func Int32Val(u int32) Val {
	return mustVal(valFromTagBody(tagI32, uint64(u)))
}

func (v Val) IsInt32() bool {
	return v.tag() == tagI32
}

func (v Val) Int32() (int32, Error) {
	if v.tag() != tagI32 {
		return 0, ErrorWrongType
	}
	body := v.body()
	u := int32(body)
	if uint64(u) != body {
		return 0, ErrorBodyMalformed
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
		return 0, ErrorWrongType
	}
	body := v.body()
	u := uint32(body)
	if uint64(u) != body {
		return 0, ErrorBodyMalformed
	}
	return u, ErrorNone
}
