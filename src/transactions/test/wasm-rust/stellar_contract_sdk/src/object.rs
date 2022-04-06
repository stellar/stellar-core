use super::val::{ValType, TAG_OBJECT};
use super::{OrAbort, Val};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Object(Val);

impl ValType for Object {
    fn is_val_type(v: Val) -> bool {
        v.has_tag(TAG_OBJECT)
    }

    unsafe fn unchecked_from_val(v: Val) -> Self {
        Object(v)
    }
}

pub trait ObjType: Into<Val> {
    fn is_obj_type(obj: Object) -> bool;
    unsafe fn unchecked_from_obj(obj: Object) -> Self;
}

impl<OB: ObjType> ValType for OB {
    fn is_val_type(v: Val) -> bool {
        v.is_object() && <Self as ObjType>::is_obj_type(v.as_object())
    }

    unsafe fn unchecked_from_val(v: Val) -> Self {
        <Self as ObjType>::unchecked_from_obj(Object(v))
    }
}

impl From<Object> for Val {
    #[inline(always)]
    fn from(obj: Object) -> Self {
        obj.0
    }
}

pub(crate) const OBJ_BOX: u8 = 0;
pub(crate) const OBJ_VEC: u8 = 1;
pub(crate) const OBJ_MAP: u8 = 2;
pub(crate) const OBJ_U64: u8 = 3;
pub(crate) const OBJ_I64: u8 = 4;
pub(crate) const OBJ_STRING: u8 = 5;
pub(crate) const OBJ_BINARY: u8 = 6;
pub(crate) const OBJ_LEDGERKEY: u8 = 7;
pub(crate) const OBJ_LEDGERVAL: u8 = 8;
pub(crate) const OBJ_OPERATION: u8 = 9;
pub(crate) const OBJ_OPERATION_RESULT: u8 = 10;
pub(crate) const OBJ_TRANSACTION: u8 = 11;
pub(crate) const OBJ_BIGNUM: u8 = 12;

impl Object {
    #[inline(always)]
    pub fn get_type(&self) -> u8 {
        (self.0.get_body() & 0xf) as u8
    }

    #[inline(always)]
    pub fn is_type(&self, ty: u8) -> bool {
        self.get_type() == ty
    }

    #[inline(always)]
    pub fn check_type(&self, ty: u8) {
        self.is_type(ty).or_abort();
    }
}
