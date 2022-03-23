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

func (t tag) Validate() {
	if t >= 0x8 {
		panic(t)
	}
}

type Val uint64

func valFromTagBody(t tag, body uint64) Val {
	t.Validate()
	if body&valBodyMask != body {
		panic(body)
	}
	return Val(uint64(t)<<48 | body)
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

func Int32Val(u int32) Val {
	return valFromTagBody(tagI32, uint64(u))
}

func (v Val) IsInt32() bool {
	return v.tag() == tagI32
}

func (v Val) Int32() int32 {
	if v.tag() != tagI32 {
		panic(v)
	}
	body := v.body()
	u := int32(body)
	if uint64(u) != body {
		panic(v)
	}
	return u
}

func Uint32Val(u uint32) Val {
	return valFromTagBody(tagU32, uint64(u))
}

func (v Val) IsUint32() bool {
	return v.tag() == tagU32
}

func (v Val) Uint32() uint32 {
	if v.tag() != tagU32 {
		panic(v)
	}
	body := v.body()
	u := uint32(body)
	if uint64(u) != body {
		panic(v)
	}
	return u
}
