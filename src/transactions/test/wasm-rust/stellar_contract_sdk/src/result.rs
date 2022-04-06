use super::{
    object::{ObjType, OBJ_OPERATION_RESULT},
    status, Object, Status, Val,
};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct OpResult(Object);

impl TryFrom<Object> for OpResult {
    type Error = Status;

    fn try_from(obj: Object) -> Result<Self, Self::Error> {
        if obj.is_type(OBJ_OPERATION_RESULT) {
            Ok(OpResult(obj))
        } else {
            Err(status::UNKNOWN_ERROR)
        }
    }
}

impl TryFrom<Val> for OpResult {
    type Error = Status;

    fn try_from(val: Val) -> Result<Self, Self::Error> {
        let obj: Object = val.try_into()?;
        if obj.is_type(OBJ_OPERATION_RESULT) {
            Ok(OpResult(obj))
        } else {
            Err(status::UNKNOWN_ERROR)
        }
    }
}

impl ObjType for OpResult {
    fn is_obj_type(obj: Object) -> bool {
        obj.is_type(OBJ_OPERATION_RESULT)
    }

    unsafe fn unchecked_from_obj(obj: Object) -> Self {
        OpResult(obj)
    }
}

impl From<OpResult> for Object {
    fn from(b: OpResult) -> Self {
        b.0
    }
}

impl From<OpResult> for Val {
    fn from(b: OpResult) -> Self {
        b.0.into()
    }
}
