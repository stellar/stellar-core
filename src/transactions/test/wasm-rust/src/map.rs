use core::marker::PhantomData;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{Object, Val, Vec};

pub struct Map<K, V>(Val, PhantomData<K>, PhantomData<V>);

impl<K: ValType, V: ValType> Map<K, V> {
    #[inline(always)]
    pub fn new() -> Map<K, V> {
        unsafe { Map(host_fns::host__map_new(), PhantomData, PhantomData) }
    }

    #[inline(always)]
    pub fn try_get(&self, k: K) -> Result<V, <V as TryFrom<Val>>::Error> {
        let v: Val = unsafe { host_fns::host__map_get(self.0, k.into()) };
        V::try_from(v)
    }

    #[inline(always)]
    pub fn get(&self, k: K) -> V {
        self.try_get(k).or_abort()
    }

    #[inline(always)]
    pub fn put(&self, k: K, v: V) -> Map<K, V> {
        let m: Val = unsafe { host_fns::host__map_put(self.0, k.into(), v.into()) };
        //m.is_object().or_abort();
        Map(m, PhantomData, PhantomData)
    }

    #[inline(always)]
    pub fn del(&self, k: K) -> Map<K, V> {
        let m: Val = unsafe { host_fns::host__map_del(self.0, k.into()) };
        //m.is_object().or_abort();
        Map(m, PhantomData, PhantomData)
    }

    #[inline(always)]
    pub fn len(&self) -> u32 {
        let m: Val = unsafe { host_fns::host__map_len(self.0) };
        m.as_u32()
    }

    #[inline(always)]
    pub fn keys(&self) -> Vec<K> {
        let v: Val = unsafe { host_fns::host__map_keys(self.0) };
        //v.is_object().or_abort();
        Vec(v, PhantomData)
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
