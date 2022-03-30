use core::marker::PhantomData;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{Object, Val};

pub struct Vec<T>(pub(crate) Val, pub(crate) PhantomData<T>);

impl<T: ValType> Vec<T> {
    #[inline(always)]
    pub fn new() -> Vec<T> {
        unsafe { Vec(host_fns::host__vec_new(), PhantomData) }
    }

    #[inline(always)]
    pub fn try_get(&self, i: u32) -> Result<T, <T as TryFrom<Val>>::Error> {
        let i: Val = i.into();
        let v: Val = unsafe { host_fns::host__vec_get(self.0, i) };
        T::try_from(v)
    }

    #[inline(always)]
    pub fn get(&self, i: u32) -> T {
        self.try_get(i).or_abort()
    }

    #[inline(always)]
    pub fn put(&self, i: u32, v: T) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_put(self.0, i.into(), v.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn del(&self, i: u32) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_del(self.0, i.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn len(&self) -> u32 {
        unsafe { host_fns::host__vec_len(self.0).as_u32() }
    }

    #[inline(always)]
    pub fn push(&self, x: T) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_push(self.0, x.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn pop(&self) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_pop(self.0) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn take(&self, n: u32) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_take(self.0, n.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn drop(&self, n: u32) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_drop(self.0, n.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn front(&self) -> T {
        let x: Val = unsafe { host_fns::host__vec_front(self.0) };
        T::try_from(x).or_abort()
    }

    #[inline(always)]
    pub fn back(&self) -> T {
        let x: Val = unsafe { host_fns::host__vec_back(self.0) };
        T::try_from(x).or_abort()
    }

    #[inline(always)]
    pub fn insert(&self, i: u32, x: T) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_insert(self.0, i.into(), x.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }

    #[inline(always)]
    pub fn append(&self, other: Vec<T>) -> Vec<T> {
        let c: Val = unsafe { host_fns::host__vec_append(self.0, other.into()) };
        c.is_object().or_abort();
        Vec(c, PhantomData)
    }
}

impl<T: ValType> From<Vec<T>> for Object {
    #[inline(always)]
    fn from(v: Vec<T>) -> Self {
        v.0.as_object()
    }
}

impl<T: ValType> From<Vec<T>> for Val {
    #[inline(always)]
    fn from(v: Vec<T>) -> Self {
        v.0
    }
}
