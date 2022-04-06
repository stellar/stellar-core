use core::marker::PhantomData;

use super::object::ObjType;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{object::OBJ_VEC, status, Object, Status, Val};

#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct Vec<T>(Object, PhantomData<T>);

impl<V: ValType> TryFrom<Object> for Vec<V> {
    type Error = Status;

    fn try_from(obj: Object) -> Result<Self, Self::Error> {
        if obj.is_type(OBJ_VEC) {
            Ok(Vec(obj, PhantomData))
        } else {
            Err(status::UNKNOWN_ERROR)
        }
    }
}

impl<V: ValType> TryFrom<Val> for Vec<V> {
    type Error = Status;

    fn try_from(val: Val) -> Result<Self, Self::Error> {
        let obj: Object = val.try_into()?;
        obj.try_into()
    }
}

impl<T: ValType> From<Vec<T>> for Object {
    #[inline(always)]
    fn from(v: Vec<T>) -> Self {
        v.0
    }
}

impl<T: ValType> From<Vec<T>> for Val {
    #[inline(always)]
    fn from(v: Vec<T>) -> Self {
        v.0.into()
    }
}

impl<V: ValType> ObjType for Vec<V> {
    fn is_obj_type(obj: Object) -> bool {
        obj.is_type(OBJ_VEC)
    }

    unsafe fn unchecked_from_obj(obj: Object) -> Self {
        Self(obj, PhantomData)
    }
}

impl<T: ValType> Vec<T> {
    unsafe fn unchecked_new(obj: Object) -> Self {
        Self(obj, PhantomData)
    }

    #[inline(always)]
    pub fn new() -> Vec<T> {
        unsafe { host_fns::vec_new().try_into().or_abort() }
    }

    #[inline(always)]
    pub fn get(&self, i: u32) -> T {
        let i: Val = i.into();
        unsafe { <T as ValType>::unchecked_from_val(host_fns::vec_get(self.0.into(), i)) }
    }

    #[inline(always)]
    pub fn put(&self, i: u32, v: T) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_put(self.0.into(), i.into(), v.into())) }
    }

    #[inline(always)]
    pub fn del(&self, i: u32) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_del(self.0.into(), i.into())) }
    }

    #[inline(always)]
    pub fn len(&self) -> u32 {
        unsafe { host_fns::vec_len(self.0.into()).as_u32() }
    }

    #[inline(always)]
    pub fn push(&self, x: T) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_push(self.0.into(), x.into())) }
    }

    #[inline(always)]
    pub fn pop(&self) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_pop(self.0.into())) }
    }

    #[inline(always)]
    pub fn take(&self, n: u32) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_take(self.0.into(), n.into())) }
    }

    #[inline(always)]
    pub fn drop(&self, n: u32) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_drop(self.0.into(), n.into())) }
    }

    #[inline(always)]
    pub fn front(&self) -> T {
        unsafe { <T as ValType>::unchecked_from_val(host_fns::vec_front(self.0.into())) }
    }

    #[inline(always)]
    pub fn back(&self) -> T {
        unsafe { <T as ValType>::unchecked_from_val(host_fns::vec_back(self.0.into())) }
    }

    #[inline(always)]
    pub fn insert(&self, i: u32, x: T) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_insert(self.0.into(), i.into(), x.into())) }
    }

    #[inline(always)]
    pub fn append(&self, other: Vec<T>) -> Vec<T> {
        unsafe { Self::unchecked_from_obj(host_fns::vec_append(self.0.into(), other.into())) }
    }
}
