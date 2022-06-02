// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#![crate_type = "staticlib"]
#![allow(non_snake_case)]

// This module (mod rust_bridge) is the signature for this crate that's exported to C++
#[cxx::bridge(namespace = "stellar::rust_bridge")]
mod rust_bridge {
    extern "Rust" {
        fn to_base64(b: &CxxVector<u8>, mut s: Pin<&mut CxxString>);
        fn from_base64(s: &CxxString, mut b: Pin<&mut CxxVector<u8>>);
        fn invoke_contract(
            wasm: &CxxVector<u8>,
            func: &CxxString,
            args: &CxxVector<u8>,
        ) -> Result<Vec<u8>>;
    }
}

// Then we import various implementations to this module, for export through the bridge.
mod b64;
use b64::{from_base64, to_base64};

mod contract;
use contract::invoke_contract;
