use crate::soroban_proto_all::protocol_agnostic::int128_helpers;
use crate::CxxI128;

pub(crate) fn i128_add(
    lhs: &CxxI128,
    rhs: &CxxI128,
) -> Result<CxxI128, Box<dyn std::error::Error>> {
    use int128_helpers::{i128_from_pieces, i128_hi, i128_lo};
    let lhs: i128 = i128_from_pieces(lhs.hi, lhs.lo);
    let rhs: i128 = i128_from_pieces(rhs.hi, rhs.lo);
    let res = lhs + rhs;
    Ok(CxxI128 {
        hi: i128_hi(res),
        lo: i128_lo(res),
    })
}

pub(crate) fn i128_sub(
    lhs: &CxxI128,
    rhs: &CxxI128,
) -> Result<CxxI128, Box<dyn std::error::Error>> {
    use int128_helpers::{i128_from_pieces, i128_hi, i128_lo};
    let lhs: i128 = i128_from_pieces(lhs.hi, lhs.lo);
    let rhs: i128 = i128_from_pieces(rhs.hi, rhs.lo);
    let res = lhs - rhs;
    Ok(CxxI128 {
        hi: i128_hi(res),
        lo: i128_lo(res),
    })
}

pub(crate) fn i128_add_will_overflow(
    lhs: &CxxI128,
    rhs: &CxxI128,
) -> Result<bool, Box<dyn std::error::Error>> {
    use int128_helpers::i128_from_pieces;
    let lhs: i128 = i128_from_pieces(lhs.hi, lhs.lo);
    let rhs: i128 = i128_from_pieces(rhs.hi, rhs.lo);

    Ok(lhs.checked_add(rhs).is_none())
}

pub(crate) fn i128_sub_will_underflow(
    lhs: &CxxI128,
    rhs: &CxxI128,
) -> Result<bool, Box<dyn std::error::Error>> {
    use int128_helpers::i128_from_pieces;
    let lhs: i128 = i128_from_pieces(lhs.hi, lhs.lo);
    let rhs: i128 = i128_from_pieces(rhs.hi, rhs.lo);

    Ok(lhs.checked_sub(rhs).is_none())
}

pub(crate) fn i128_from_i64(val: i64) -> Result<CxxI128, Box<dyn std::error::Error>> {
    use int128_helpers::{i128_hi, i128_lo};
    let res = i128::from(val);
    Ok(CxxI128 {
        hi: i128_hi(res),
        lo: i128_lo(res),
    })
}

pub(crate) fn i128_is_negative(val: &CxxI128) -> Result<bool, Box<dyn std::error::Error>> {
    use int128_helpers::i128_from_pieces;
    let res: i128 = i128_from_pieces(val.hi, val.lo);
    Ok(res.is_negative())
}
