use core::{
    cmp::Ordering,
    ops::{Add, BitAnd, BitOr, BitXor, Div, Mul, Neg, Not, Rem, Shl, Shr, Sub},
};

use super::{host_fns, object::OBJ_BIGNUM, or_abort::OrAbort, Object, Status, Val};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct BigNum(Object);

impl TryFrom<Object> for BigNum {
    type Error = Status;

    fn try_from(obj: Object) -> Result<Self, Self::Error> {
        if obj.is_type(OBJ_BIGNUM) {
            Ok(BigNum(obj))
        } else {
            Err(Status(0))
        }
    }
}

impl TryFrom<Val> for BigNum {
    type Error = Status;

    fn try_from(val: Val) -> Result<Self, Self::Error> {
        let obj: Object = val.try_into()?;
        if obj.is_type(OBJ_BIGNUM) {
            Ok(BigNum(obj))
        } else {
            Err(Status(0))
        }
    }
}

impl From<BigNum> for Object {
    fn from(b: BigNum) -> Self {
        b.0
    }
}

impl From<BigNum> for Val {
    fn from(b: BigNum) -> Self {
        b.0.into()
    }
}

impl From<u64> for BigNum {
    fn from(x: u64) -> Self {
        let b = unsafe { host_fns::host__bignum_from_u64(x) };
        b.try_into().or_abort()
    }
}

impl Add for BigNum {
    type Output = BigNum;
    fn add(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_add(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl Sub for BigNum {
    type Output = BigNum;
    fn sub(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_sub(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl Mul for BigNum {
    type Output = BigNum;
    fn mul(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_mul(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl Div for BigNum {
    type Output = BigNum;
    fn div(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_div(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl Rem for BigNum {
    type Output = BigNum;
    fn rem(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_rem(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl BitAnd for BigNum {
    type Output = BigNum;
    fn bitand(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_and(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl BitOr for BigNum {
    type Output = BigNum;
    fn bitor(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_or(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl BitXor for BigNum {
    type Output = BigNum;
    fn bitxor(self, rhs: Self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_xor(self.into(), rhs.into()) };
        b.try_into().or_abort()
    }
}

impl Neg for BigNum {
    type Output = BigNum;
    fn neg(self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_neg(self.into()) };
        b.try_into().or_abort()
    }
}

impl Not for BigNum {
    type Output = BigNum;
    fn not(self) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_not(self.into()) };
        b.try_into().or_abort()
    }
}

impl Shl<u64> for BigNum {
    type Output = BigNum;
    fn shl(self, rhs: u64) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_shl(self.into(), rhs) };
        b.try_into().or_abort()
    }
}

impl Shr<u64> for BigNum {
    type Output = BigNum;
    fn shr(self, rhs: u64) -> Self::Output {
        let b = unsafe { host_fns::host__bignum_shr(self.into(), rhs) };
        b.try_into().or_abort()
    }
}

impl PartialEq for BigNum {
    fn eq(&self, other: &Self) -> bool {
        self.partial_cmp(other) == Some(Ordering::Equal)
    }
}

impl PartialOrd for BigNum {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(Ord::cmp(self, other))
    }
}

impl Eq for BigNum {}
impl Ord for BigNum {
    fn cmp(&self, other: &Self) -> Ordering {
        let v = unsafe { host_fns::host__bignum_cmp((*self).into(), (*other).into()) };
        let i = v.as_i32();
        if i < 0 {
            Ordering::Less
        } else if i > 0 {
            Ordering::Greater
        } else {
            Ordering::Equal
        }
    }
}

impl BigNum {
    pub fn gcd(&self, other: BigNum) -> BigNum {
        let b = unsafe { host_fns::host__bignum_gcd((*self).into(), other.into()) };
        b.try_into().or_abort()
    }

    pub fn lcm(&self, other: BigNum) -> BigNum {
        let b = unsafe { host_fns::host__bignum_lcm((*self).into(), other.into()) };
        b.try_into().or_abort()
    }

    pub fn pow(&self, k: u64) -> BigNum {
        let b = unsafe { host_fns::host__bignum_pow((*self).into(), k) };
        b.try_into().or_abort()
    }

    pub fn pow_mod(&self, q: BigNum, m: BigNum) -> BigNum {
        let b = unsafe { host_fns::host__bignum_pow_mod((*self).into(), q.into(), m.into()) };
        b.try_into().or_abort()
    }

    pub fn sqrt(&self) -> BigNum {
        let b = unsafe { host_fns::host__bignum_sqrt((*self).into()) };
        b.try_into().or_abort()
    }

    pub fn is_zero(&self) -> bool {
        let b = unsafe { host_fns::host__bignum_is_zero((*self).into()) };
        b.try_into().or_abort()
    }
}
