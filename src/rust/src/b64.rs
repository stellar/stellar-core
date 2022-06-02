// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use base64::DecodeError;
use cxx::{CxxString, CxxVector};
use std::{borrow::Cow, pin::Pin};

pub(crate) fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>) {
    s.as_mut().clear();
    s.push_str(base64::encode(b.as_slice()).as_str());
}

pub(crate) fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>) {
    assert!(b.len() == 0);

    let config =
        base64::Config::new(base64::CharacterSet::Standard, true).decode_allow_trailing_bits(true);

    let mut cow = s.to_string_lossy();
    let bb = loop {
        if cow.is_empty() {
            return;
        }
        match base64::decode_config(cow.as_ref(), config) {
            Ok(bb) => break bb,
            Err(e) => {
                let mut s = cow.to_string();
                match e {
                    DecodeError::InvalidByte(pos, _) | DecodeError::InvalidLastSymbol(pos, _) => {
                        s.remove(pos);
                    }
                    DecodeError::InvalidLength => {
                        s.pop();
                    }
                };
                if s.is_empty() {
                    return;
                }
                cow = Cow::Owned(s);
            }
        }
    };

    for i in bb {
        b.as_mut().push(i)
    }
}
