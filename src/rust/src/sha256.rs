// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// SHA-256 implemented via the RustCrypto `sha2` crate, exposed to C++ over the
// cxx bridge. Unlike OpenSSL 3.x's one-shot `SHA256()` (which takes a global
// algorithm-fetch lock per call and therefore does not scale across threads),
// this implementation is stateless, lock-free, and uses runtime CPU feature
// detection (SHA-NI), so it both scales across threads and is fast.

use sha2::{Digest, Sha256};

// Computes SHA-256 of `data_len` bytes at `data` and writes the 32-byte digest
// to `out`. `out` must point to at least 32 writable bytes.
//
// # Safety
// `data`/`data_len` must describe a valid readable region (or len 0), and `out`
// must point to 32 writable bytes.
pub(crate) unsafe fn sha256_rust(data: *const u8, data_len: usize, out: *mut u8) {
    let input: &[u8] = if data_len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(data, data_len)
    };
    let digest = Sha256::digest(input);
    std::ptr::copy_nonoverlapping(digest.as_ptr(), out, 32);
}

// Incremental SHA-256 state, exposed to C++ as an opaque cxx type so that
// callers (e.g. xdrSha256) can stream bytes in without first materializing a
// contiguous buffer. Same lock-free, thread-scalable properties as the one-shot
// above.
pub(crate) struct RustSha256 {
    hasher: Sha256,
}

pub(crate) fn new_rust_sha256() -> Box<RustSha256> {
    Box::new(RustSha256 {
        hasher: Sha256::new(),
    })
}

impl RustSha256 {
    // # Safety: `data`/`len` must describe a valid readable region (or len 0).
    pub(crate) unsafe fn update(&mut self, data: *const u8, len: usize) {
        let input: &[u8] = if len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(data, len)
        };
        self.hasher.update(input);
    }

    // # Safety: `out` must point to 32 writable bytes. Resets the hasher.
    pub(crate) unsafe fn finalize(&mut self, out: *mut u8) {
        let digest = self.hasher.finalize_reset();
        std::ptr::copy_nonoverlapping(digest.as_ptr(), out, 32);
    }
}
