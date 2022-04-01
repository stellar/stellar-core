use core::marker::PhantomData;

use super::host_fns;
use super::val::ValType;
use super::OrAbort;
use super::{Object, Val, Vec, Status, object::OBJ_MAP};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Map<K, V>(Object, PhantomData<K>, PhantomData<V>);

impl<K:ValType,V:ValType> TryFrom<Object> for Map<K,V> {
    type Error = Status;

    fn try_from(obj: Object) -> Result<Self, Self::Error> {
        if obj.is_type(OBJ_MAP) {
            Ok(Map(obj, PhantomData, PhantomData))
        } else {
            Err(Status(0))
        }
    }
}

impl<K:ValType,V:ValType> TryFrom<Val> for Map<K,V> {
    type Error = Status;

    fn try_from(val: Val) -> Result<Self, Self::Error> {
        let obj:Object = val.try_into()?;
        obj.try_into()
    }
}

impl<K: ValType, V: ValType> From<Map<K, V>> for Object {
    #[inline(always)]
    fn from(m: Map<K, V>) -> Self {
        m.0
    }
}

impl<K: ValType, V: ValType> From<Map<K, V>> for Val {
    #[inline(always)]
    fn from(m: Map<K, V>) -> Self {
        m.0.into()
    }
}

impl<K: ValType, V: ValType> Map<K, V> {
    #[inline(always)]
    pub fn new() -> Map<K, V> {
        unsafe { host_fns::host__map_new().try_into().or_abort() }
    }

    #[inline(always)]
    pub fn try_get(&self, k: K) -> Result<V, <V as TryFrom<Val>>::Error> {
        let v: Val = unsafe { host_fns::host__map_get(self.0.into(), k.into()) };
        V::try_from(v)
    }

    #[inline(always)]
    pub fn get(&self, k: K) -> V {
        self.try_get(k).or_abort()
    }

    #[inline(always)]
    pub fn put(&self, k: K, v: V) -> Map<K, V> {
        let m: Val = unsafe { host_fns::host__map_put(self.0.into(), k.into(), v.into()) };
        m.try_into().or_abort()
    }

    #[inline(always)]
    pub fn del(&self, k: K) -> Map<K, V> {
        let m: Val = unsafe { host_fns::host__map_del(self.0.into(), k.into()) };
        m.try_into().or_abort()
    }

    #[inline(always)]
    pub fn len(&self) -> u32 {
        let m: Val = unsafe { host_fns::host__map_len(self.0.into()) };
        m.as_u32()
    }

    #[inline(always)]
    pub fn keys(&self) -> Vec<K> {
        let v: Val = unsafe { host_fns::host__map_keys(self.0.into()) };
        v.try_into().or_abort()
    }
}
