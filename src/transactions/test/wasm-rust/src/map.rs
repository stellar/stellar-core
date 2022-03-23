use std::marker::PhantomData;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{Object, Val};

pub struct Map<K, V>(Val, PhantomData<K>, PhantomData<V>);

impl<K: ValType, V: ValType> Map<K, V> {
    #[inline(always)]
    pub fn new() -> Map<K, V> {
        unsafe {
            Map(
                Val::from_payload(host_fns::map_new()),
                PhantomData,
                PhantomData,
            )
        }
    }

    #[inline(always)]
    pub fn try_get(&self, k: K) -> Result<V, <V as TryFrom<Val>>::Error> {
        unsafe {
            let k: Val = k.into();
            let r: u64 = host_fns::map_get(self.0.payload(), k.payload());
            let v: Val = Val::from_payload(r);
            V::try_from(v)
        }
    }

    #[inline(always)]
    pub fn get(&self, k: K) -> V {
        unsafe {
            let k: Val = k.into();
            let r: u64 = host_fns::map_get(self.0.payload(), k.payload());
            let v: Val = Val::from_payload(r);
            V::try_from(v).or_abort()
        }
    }

    #[inline(always)]
    pub fn put(&self, k: K, v: V) -> Map<K, V> {
        unsafe {
            let k: Val = k.into();
            let v: Val = v.into();
            let r: u64 = host_fns::map_put(self.0.payload(), k.payload(), v.payload());
            Map(Val::from_payload(r), PhantomData, PhantomData)
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
