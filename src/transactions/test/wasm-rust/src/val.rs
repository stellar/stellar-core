use super::OrAbort;

const OBJ_VOID: u64 = 0;
const OBJ_BOOL_TRUE: u64 = 1;
const OBJ_BOOL_FALSE: u64 = 2;

const TAG_OBJECT: u16 = 0;
const TAG_U32: u16 = 1;
const TAG_I32: u16 = 2;
const TAG_SYMBOL: u16 = 3;
const TAG_BITSET: u16 = 4;
const TAG_TIMEPT: u16 = 5;
const TAG_STATUS: u16 = 6;

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Val(u64);

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Symbol(pub(crate) u64);

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct BitSet(u64);

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct TimePt(u64);

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Object(u64);

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Error(u32);

pub trait ValType: Into<Val> + TryFrom<Val> {}
impl ValType for () {}
impl ValType for bool {}
impl ValType for u32 {}
impl ValType for i32 {}
impl ValType for Symbol {}
impl ValType for BitSet {}
impl ValType for TimePt {}
impl ValType for Object {}

impl From<Symbol> for Val {
    #[inline(always)]
    fn from(s: Symbol) -> Self {
        Val::from_symbol(s)
    }
}

impl TryFrom<Val> for Symbol {
    type Error = Error;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_symbol() {
            Ok(value.as_symbol())
        } else {
            Err(Error(0))
        }
    }
}

impl From<bool> for Val {
    #[inline(always)]
    fn from(b: bool) -> Self {
        Val::from_bool(b)
    }
}

impl TryFrom<Val> for bool {
    type Error = Error;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_bool() {
            Ok(value.as_bool())
        } else {
            Err(Error(0))
        }
    }
}

impl From<()> for Val {
    #[inline(always)]
    fn from(_: ()) -> Self {
        Val::from_void()
    }
}

impl TryFrom<Val> for () {
    type Error = Error;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_void() {
            Ok(())
        } else {
            Err(Error(0))
        }
    }
}

impl From<u32> for Val {
    #[inline(always)]
    fn from(u: u32) -> Self {
        Val::from_u32(u)
    }
}

impl TryFrom<Val> for u32 {
    type Error = Error;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_u32() {
            Ok(value.as_u32())
        } else {
            Err(Error(0))
        }
    }
}

impl From<i32> for Val {
    #[inline(always)]
    fn from(i: i32) -> Self {
        Val::from_i32(i)
    }
}

impl TryFrom<Val> for i32 {
    type Error = Error;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_i32() {
            Ok(value.as_i32())
        } else {
            Err(Error(0))
        }
    }
}

impl From<BitSet> for Val {
    #[inline(always)]
    fn from(b: BitSet) -> Self {
        Val::from_bit_set(b)
    }
}

impl TryFrom<Val> for BitSet {
    type Error = Error;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_bit_set() {
            Ok(value.as_bit_set())
        } else {
            Err(Error(0))
        }
    }
}

impl From<TimePt> for Val {
    #[inline(always)]
    fn from(t: TimePt) -> Self {
        Val::from_time_pt(t)
    }
}

impl TryFrom<Val> for TimePt {
    type Error = Error;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_time_pt() {
            Ok(value.as_time_pt())
        } else {
            Err(Error(0))
        }
    }
}

impl From<Object> for Val {
    #[inline(always)]
    fn from(obj: Object) -> Self {
        Val::from_object(obj)
    }
}
impl TryFrom<Val> for Object {
    type Error = Error;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_object() {
            Ok(value.as_object())
        } else {
            Err(Error(0))
        }
    }
}

impl Val {
    #[inline(always)]
    fn get_tag(&self) -> u16 {
        self.0 as u16
    }

    #[inline(always)]
    fn get_body(&self) -> u64 {
        self.0 >> 16
    }

    #[inline(always)]
    fn has_tag(&self, tag: u16) -> bool {
        self.get_tag() == tag
    }

    #[inline(always)]
    pub fn is_void(&self) -> bool {
        self.has_tag(TAG_OBJECT) && self.get_body() == OBJ_VOID
    }

    #[inline(always)]
    pub fn is_bool(&self) -> bool {
        self.has_tag(TAG_OBJECT)
            && (self.get_body() == OBJ_BOOL_TRUE || self.get_body() == OBJ_BOOL_FALSE)
    }

    #[inline(always)]
    pub fn as_bool(&self) -> bool {
        self.is_bool().or_abort();
        self.get_body() == OBJ_BOOL_TRUE
    }

    #[inline(always)]
    pub fn is_status(&self) -> bool {
        self.has_tag(TAG_STATUS)
    }

    #[inline(always)]
    pub fn as_status(&self) -> u32 {
        self.is_status().or_abort();
        self.get_body() as u32
    }

    #[inline(always)]
    pub fn is_u32(&self) -> bool {
        self.has_tag(TAG_U32)
    }

    #[inline(always)]
    pub fn as_u32(&self) -> u32 {
        self.is_u32().or_abort();
        self.get_body() as u32
    }

    #[inline(always)]
    pub fn is_i32(&self) -> bool {
        self.has_tag(TAG_I32)
    }

    #[inline(always)]
    pub fn as_i32(&self) -> i32 {
        self.is_i32().or_abort();
        self.get_body() as i32
    }

    #[inline(always)]
    pub fn is_symbol(&self) -> bool {
        self.has_tag(TAG_SYMBOL)
    }

    #[inline(always)]
    pub fn as_symbol(&self) -> Symbol {
        self.is_symbol().or_abort();
        Symbol(self.get_body())
    }

    #[inline(always)]
    pub fn is_bit_set(&self) -> bool {
        self.has_tag(TAG_BITSET)
    }

    #[inline(always)]
    pub fn as_bit_set(&self) -> BitSet {
        self.is_bit_set().or_abort();
        BitSet(self.get_body())
    }

    #[inline(always)]
    pub fn is_time_pt(&self) -> bool {
        self.has_tag(TAG_TIMEPT)
    }

    #[inline(always)]
    pub fn as_time_pt(&self) -> TimePt {
        self.is_time_pt().or_abort();
        TimePt(self.get_body())
    }

    #[inline(always)]
    pub fn is_object(&self) -> bool {
        self.has_tag(TAG_OBJECT)
    }

    #[inline(always)]
    pub fn as_object(&self) -> Object {
        self.is_object().or_abort();
        Object(self.get_body())
    }

    #[inline(always)]
    fn from_tag_and_body(tag: u16, body: u64) -> Val {
        (tag < 7).or_abort();
        let body = body.rotate_left(16);
        ((body & 0xffffu64) == 0).or_abort();
        Val(body | (tag as u64))
    }

    #[inline(always)]
    pub fn from_void() -> Val {
        Val::from_tag_and_body(TAG_OBJECT, OBJ_VOID)
    }

    #[inline(always)]
    pub fn from_bool(b: bool) -> Val {
        Val::from_tag_and_body(TAG_OBJECT, if b { OBJ_BOOL_TRUE } else { OBJ_BOOL_FALSE })
    }

    #[inline(always)]
    pub fn from_status(e: u32) -> Val {
        Val::from_tag_and_body(TAG_STATUS, e as u64)
    }

    #[inline(always)]
    pub fn from_u32(u: u32) -> Val {
        Val::from_tag_and_body(TAG_U32, u as u64)
    }

    #[inline(always)]
    pub fn from_i32(i: i32) -> Val {
        Val::from_tag_and_body(TAG_I32, (i as u32) as u64)
    }

    #[inline(always)]
    pub fn from_symbol(s: Symbol) -> Val {
        Val::from_tag_and_body(TAG_SYMBOL, s.0)
    }

    #[inline(always)]
    pub fn from_bit_set(bits: BitSet) -> Val {
        Val::from_tag_and_body(TAG_BITSET, bits.0)
    }

    #[inline(always)]
    pub fn from_time_pt(time: TimePt) -> Val {
        Val::from_tag_and_body(TAG_TIMEPT, time.0)
    }

    #[inline(always)]
    pub fn from_object(obj: Object) -> Val {
        Val::from_tag_and_body(TAG_OBJECT, obj.0)
    }
}
