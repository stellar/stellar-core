use super::OrAbort;
use super::Object;

const TAG_U32: u8 = 0;
const TAG_I32: u8 = 1;
const TAG_STATIC: u8 = 2;
const TAG_OBJECT: u8 = 3;
const TAG_SYMBOL: u8 = 4;
const TAG_BITSET: u8 = 5;
const TAG_STATUS: u8 = 6;

const STATIC_VOID: u32 = 0;
const STATIC_TRUE: u32 = 1;
const STATIC_FALSE: u32 = 2;

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
pub struct Status(pub(crate) u64);

pub trait ValType: Into<Val> + TryFrom<Val> {}
impl ValType for () {}
impl ValType for bool {}
impl ValType for u32 {}
impl ValType for i32 {}
impl ValType for Symbol {}
impl ValType for BitSet {}
impl ValType for Object {}

impl TryFrom<i64> for Val {
    type Error = Status;

    #[inline(always)]
    fn try_from(i: i64) -> Result<Self, Self::Error> {
        if i > 0 {
            Ok(Val::from_u63(i))
        } else {
            Err(Status(0))
        }
    }
}

impl TryFrom<Val> for i64 {
    type Error = Status;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_u63() {
            Ok(value.as_u63())
        } else {
            Err(Status(0))
        }
    }
}

impl From<Symbol> for Val {
    #[inline(always)]
    fn from(s: Symbol) -> Self {
        Val::from_symbol(s)
    }
}

impl TryFrom<Val> for Symbol {
    type Error = Status;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_symbol() {
            Ok(value.as_symbol())
        } else {
            Err(Status(0))
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
    type Error = Status;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_bool() {
            Ok(value.as_bool())
        } else {
            Err(Status(0))
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
    type Error = Status;

    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_void() {
            Ok(())
        } else {
            Err(Status(0))
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
    type Error = Status;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_u32() {
            Ok(value.as_u32())
        } else {
            Err(Status(0))
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
    type Error = Status;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_i32() {
            Ok(value.as_i32())
        } else {
            Err(Status(0))
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
    type Error = Status;
    #[inline(always)]
    fn try_from(value: Val) -> Result<Self, Self::Error> {
        if value.is_bit_set() {
            Ok(value.as_bit_set())
        } else {
            Err(Status(0))
        }
    }
}

impl Val {

    #[inline(always)]
    fn is_u63(&self) -> bool {
        let is = (self.0 & 1) == 0;
        /*
        if is {
            super::log_value(Symbol::from_str("is_u63").into());
        } else {
            super::log_value(Symbol::from_str("is_not_u63").into());
        }
        */
        is
    }

    #[inline(always)]
    fn as_u63(&self) -> i64 {
        (self.0 >> 1) as i64
    }

    #[inline(always)]
    pub(crate) fn get_tag(&self) -> u8 {
        //(!self.is_u63()).or_abort();
        //super::log_value(Symbol::from_str("get_tag").into());
        ((self.0 >> 1) & 7) as u8
    }

    #[inline(always)]
    pub(crate) fn get_body(&self) -> u64 {
        //(!self.is_u63()).or_abort();
        //super::log_value(Symbol::from_str("get_body").into());
        self.0 >> 4
    }

    #[inline(always)]
    fn has_tag(&self, tag: u8) -> bool {
        //super::log_value(Symbol::from_str("has_tag").into());
        !self.is_u63() && self.get_tag() == tag
    }

    #[inline(always)]
    pub fn is_void(&self) -> bool {
        self.has_tag(TAG_STATIC) && self.get_body() == STATIC_VOID as u64
    }

    #[inline(always)]
    pub fn is_bool(&self) -> bool {
        self.has_tag(TAG_STATIC)
            && (self.get_body() == STATIC_TRUE as u64 || self.get_body() == STATIC_FALSE as u64)
    }

    #[inline(always)]
    pub fn as_bool(&self) -> bool {
        self.is_bool().or_abort();
        self.get_body() == STATIC_TRUE as u64
    }

    #[inline(always)]
    pub fn is_status(&self) -> bool {
        self.has_tag(TAG_STATUS)
    }

    #[inline(always)]
    pub fn as_status(&self) -> Status {
        self.is_status().or_abort();
        Status(self.get_body())
    }

    #[inline(always)]
    pub fn is_u32(&self) -> bool {
        //super::log_value(Symbol::from_str("is_u32").into());
        self.has_tag(TAG_U32)
    }

    #[inline(always)]
    pub fn as_u32(&self) -> u32 {
        //super::log_value(Symbol::from_str("as_u32_a").into());
        self.is_u32().or_abort();
        //super::log_value(Symbol::from_str("as_u32_b").into());
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
    pub fn is_object(&self) -> bool {
        self.has_tag(TAG_OBJECT)
    }

    #[inline(always)]
    pub fn as_object(&self) -> Object {
        (*self).try_into().or_abort()
    }

    #[inline(always)]
    pub fn from_u63(i: i64) -> Val {
        (i >= 0).or_abort();
        Val((i as u64) << 1)
    }

    #[inline(always)]
    // This does no checking, so it can be used in const fns
    // below; it should not be made public.
    const fn from_body_and_tag(body: u64, tag: u8) -> Val {
        Val(body << 4 | ((tag << 1) as u64) | 1)
    }

    #[inline(always)]
    pub const fn from_void() -> Val {
        Val::from_body_and_tag(STATIC_VOID as u64, TAG_STATIC)
    }

    #[inline(always)]
    pub const fn from_bool(b: bool) -> Val {
        let body = if b { STATIC_TRUE } else { STATIC_FALSE };
        Val::from_body_and_tag(body as u64, TAG_STATIC)
    }

    #[inline(always)]
    pub const fn from_status(e: u32) -> Val {
        Val::from_body_and_tag(e as u64, TAG_STATUS)
    }

    #[inline(always)]
    pub const fn from_u32(u: u32) -> Val {
        Val::from_body_and_tag(u as u64, TAG_U32)
    }

    #[inline(always)]
    pub const fn from_i32(i: i32) -> Val {
        Val::from_body_and_tag((i as u32) as u64, TAG_I32)
    }

    #[inline(always)]
    pub const fn from_symbol(s: Symbol) -> Val {
        Val::from_body_and_tag(s.0, TAG_SYMBOL)
    }

    #[inline(always)]
    pub fn from_bit_set(bits: BitSet) -> Val {
        Val::from_body_and_tag(bits.0, TAG_BITSET)
    }

    #[inline(always)]
    pub fn from_object(obj: Object) -> Val {
        obj.into()
    }
}
