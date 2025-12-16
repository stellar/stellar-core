use cxx::{CxxString, CxxVector};
use std::str::FromStr;
use stellar_xdr::curr;

fn to_json(buf: &CxxVector<u8>, xdr_type: &CxxString, compact: bool) -> Option<String> {
    let xdr_type = curr::TypeVariant::from_str(xdr_type.to_str().ok()?).ok()?;
    let item = curr::Type::read_xdr_to_end(
        xdr_type,
        &mut curr::Limited::new(buf.as_slice(), curr::Limits::none()),
    )
    .ok()?;
    if compact {
        serde_json::to_string(&item).ok()
    } else {
        serde_json::to_string_pretty(&item).ok()
    }
}

pub(crate) fn xdr_to_json(buf: &CxxVector<u8>, xdr_type: &CxxString, compact: bool) -> String {
    to_json(buf, xdr_type, compact).unwrap_or(String::new())
}
