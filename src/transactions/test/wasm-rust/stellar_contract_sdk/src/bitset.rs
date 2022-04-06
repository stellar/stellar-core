use super::val::{ValType, TAG_BITSET};
use super::Val;

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct BitSet(Val);

impl ValType for BitSet {
    fn is_val_type(v: Val) -> bool {
        v.has_tag(TAG_BITSET)
    }

    unsafe fn unchecked_from_val(v: Val) -> Self {
        BitSet(v)
    }
}

impl From<BitSet> for Val {
    #[inline(always)]
    fn from(b: BitSet) -> Self {
        b.0
    }
}
