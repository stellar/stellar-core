use core::marker::PhantomData;

use super::host_fns;
use super::object::ObjType;
use super::val::ValType;
use super::{object::OBJ_MAP, status, Object, Status, Val, Vec};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Map<K, V>(Object, PhantomData<K>, PhantomData<V>);

impl<K: ValType, V: ValType> TryFrom<Object> for Map<K, V> {
    type Error = Status;

    fn try_from(obj: Object) -> Result<Self, Self::Error> {
        if obj.is_type(OBJ_MAP) {
            Ok(Map(obj, PhantomData, PhantomData))
        } else {
            Err(status::UNKNOWN_ERROR)
        }
    }
}

impl<K: ValType, V: ValType> TryFrom<Val> for Map<K, V> {
    type Error = Status;

    fn try_from(val: Val) -> Result<Self, Self::Error> {
        let obj: Object = val.try_into()?;
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

impl<K: ValType, V: ValType> ObjType for Map<K, V> {
    fn is_obj_type(obj: Object) -> bool {
        obj.is_type(OBJ_MAP)
    }

    unsafe fn unchecked_from_obj(obj: Object) -> Self {
        Map(obj, PhantomData, PhantomData)
    }
}

impl<K: ValType, V: ValType> Map<K, V> {
    #[inline(always)]
    pub fn new() -> Map<K, V> {
        unsafe { Self::unchecked_from_obj(host_fns::map_new()) }
    }

    #[inline(always)]
    pub fn has(&self, k: K) -> bool {
        unsafe { <bool as ValType>::unchecked_from_val(host_fns::map_has(self.0.into(), k.into())) }
    }

    #[inline(always)]
    pub fn get(&self, k: K) -> V {
        unsafe { <V as ValType>::unchecked_from_val(host_fns::map_get(self.0.into(), k.into())) }
    }

    #[inline(always)]
    pub fn put(&self, k: K, v: V) -> Map<K, V> {
        unsafe { Self::unchecked_from_obj(host_fns::map_put(self.0.into(), k.into(), v.into())) }
    }

    #[inline(always)]
    pub fn del(&self, k: K) -> Map<K, V> {
        unsafe { Self::unchecked_from_obj(host_fns::map_del(self.0.into(), k.into())) }
    }

    #[inline(always)]
    pub fn len(&self) -> u32 {
        let m: Val = unsafe { host_fns::map_len(self.0.into()) };
        m.as_u32()
    }

    #[inline(always)]
    pub fn keys(&self) -> Vec<K> {
        unsafe { <Vec<K> as ObjType>::unchecked_from_obj(host_fns::map_keys(self.0.into())) }
    }
}
