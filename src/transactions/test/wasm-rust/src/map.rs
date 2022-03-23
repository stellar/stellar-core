use std::marker::PhantomData;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{Object, Val};

pub struct Map<K, V>(Val, PhantomData<K>, PhantomData<V>);

impl<K: ValType, V: ValType> Map<K, V> {
    #[inline(always)]
    pub fn new() -> Map<K, V> {
        unsafe { Map(host_fns::map_new(), PhantomData, PhantomData) }
    }

    #[inline(always)]
    pub fn try_get(&self, k: K) -> Result<V, <V as TryFrom<Val>>::Error> {
        unsafe {
            let k: Val = k.into();
            let v: Val = host_fns::map_get(self.0, k);
            V::try_from(v)
        }
    }

    #[inline(always)]
    pub fn get(&self, k: K) -> V {
        self.try_get(k).or_abort()
    }

    #[inline(always)]
    pub fn put(&self, k: K, v: V) -> Map<K, V> {
        unsafe {
            let k: Val = k.into();
            let v: Val = v.into();
            let m: Val = host_fns::map_put(self.0, k, v);
            Map(m, PhantomData, PhantomData)
        }
    }
}

impl<K: ValType, V: ValType> From<Map<K, V>> for Object {
    #[inline(always)]
    fn from(m: Map<K, V>) -> Self {
        m.0.as_object()
    }
}

impl<K: ValType, V: ValType> From<Map<K, V>> for Val {
    #[inline(always)]
    fn from(m: Map<K, V>) -> Self {
        m.0
    }
}
