use core::marker::PhantomData;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{object::OBJ_VEC, Object, Status, Val};

#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct Vec<T>(Object, PhantomData<T>);

impl<V: ValType> TryFrom<Object> for Vec<V> {
    type Error = Status;

    fn try_from(obj: Object) -> Result<Self, Self::Error> {
        if obj.is_type(OBJ_VEC) {
            Ok(Vec(obj, PhantomData))
        } else {
            Err(Status(0))
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

impl<T: ValType> Vec<T> {
    #[inline(always)]
    pub fn new() -> Vec<T> {
        unsafe { host_fns::host__vec_new().try_into().or_abort() }
    }

    #[inline(always)]
    pub fn try_get(&self, i: u32) -> Result<T, <T as TryFrom<Val>>::Error> {
        let i: Val = i.into();
        let v: Val = unsafe { host_fns::host__vec_get(self.0.into(), i) };
        T::try_from(v)
    }

    #[inline(always)]
    pub fn get(&self, i: u32) -> T {
        self.try_get(i).or_abort()
    }

    #[inline(always)]
    pub fn put(&self, i: u32, v: T) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_put(self.0.into(), i.into(), v.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn del(&self, i: u32) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_del(self.0.into(), i.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn len(&self) -> u32 {
        unsafe { host_fns::host__vec_len(self.0.into()).as_u32() }
    }

    #[inline(always)]
    pub fn push(&self, x: T) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_push(self.0.into(), x.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn pop(&self) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_pop(self.0.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn take(&self, n: u32) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_take(self.0.into(), n.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn drop(&self, n: u32) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_drop(self.0.into(), n.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn front(&self) -> T {
        let x: Val = unsafe { host_fns::host__vec_front(self.0.into()) };
        T::try_from(x).or_abort()
    }

    #[inline(always)]
    pub fn back(&self) -> T {
        let x: Val = unsafe { host_fns::host__vec_back(self.0.into()) };
        T::try_from(x).or_abort()
    }

    #[inline(always)]
    pub fn insert(&self, i: u32, x: T) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_insert(self.0.into(), i.into(), x.into()) };
        v.try_into().or_abort()
    }

    #[inline(always)]
    pub fn append(&self, other: Vec<T>) -> Vec<T> {
        let v: Val = unsafe { host_fns::host__vec_append(self.0.into(), other.into()) };
        v.try_into().or_abort()
    }
}
