#![crate_type = "staticlib"]
#![allow(non_snake_case)]

use base64::DecodeError;
use cxx::{CxxString, CxxVector};
use std::{borrow::Cow, pin::Pin};

// This module (mod bridge) is the signature for this crate that's exported to C++
#[cxx::bridge(namespace = "stellar::bridge")]
mod bridge {
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
    }
}

// This is the rust implementation of the bridge signature.
fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>) {
    s.as_mut().clear();
    s.push_str(base64::encode(b.as_slice()).as_str());
}

fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>) {
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
