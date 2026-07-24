// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Rust global allocator backed by the C++ tcmalloc (gperftools) allocator that
//! is linked into stellar-core. This is only compiled with the `tcmalloc`
//! feature, which the build system enables whenever the C++ build uses tcmalloc
//! (see `USE_TCMALLOC` in configure.ac / src/Makefile.am). Builds without
//! tcmalloc (sanitizer or non-Linux builds) simply use Rust's default
//! allocator.
//!
//! The `tc_*` symbols are provided by `libtcmalloc_minimal.a` and are left
//! undefined in `librust_stellar_core.a`; they are resolved at the final C++
//! link step, exactly like the `shim_*` C++ functions imported by the bridge.
//! No `#[link]` attribute or build script is required.

use core::ffi::c_void;
use std::alloc::{GlobalAlloc, Layout};

extern "C" {
    fn tc_malloc(size: usize) -> *mut c_void;
    fn tc_free(ptr: *mut c_void);
    fn tc_realloc(ptr: *mut c_void, size: usize) -> *mut c_void;
    fn tc_calloc(nmemb: usize, size: usize) -> *mut c_void;
    fn tc_memalign(alignment: usize, size: usize) -> *mut c_void;
}

// tcmalloc, like the system malloc, guarantees alignment suitable for any
// fundamental type (alignof(max_align_t)): 16 bytes on 64-bit, 8 on 32-bit.
// Requests with a larger alignment need to go through tc_memalign.
#[cfg(target_pointer_width = "64")]
const MIN_ALIGN: usize = 16;
#[cfg(target_pointer_width = "32")]
const MIN_ALIGN: usize = 8;

pub struct TcMalloc;

// This implementation mirrors the standard library's unix allocator (
// https://github.com/rust-lang/rust/blob/225e91c03da22cd4b9792b83c1cfc97967101614/library/std/src/sys/alloc/unix.rs).
// Normally aligned allocations use tc_malloc/tc_calloc/tc_realloc, over-aligned
// ones use tc_memalign, and tc_free deallocates memory from any of these.
unsafe impl GlobalAlloc for TcMalloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= MIN_ALIGN && layout.align() <= layout.size() {
            tc_malloc(layout.size()) as *mut u8
        } else {
            tc_memalign(layout.align(), layout.size()) as *mut u8
        }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= MIN_ALIGN && layout.align() <= layout.size() {
            tc_calloc(1, layout.size()) as *mut u8
        } else {
            let ptr = self.alloc(layout);
            if !ptr.is_null() {
                core::ptr::write_bytes(ptr, 0, layout.size());
            }
            ptr
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        tc_free(ptr as *mut c_void);
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= MIN_ALIGN && layout.align() <= new_size {
            tc_realloc(ptr as *mut c_void, new_size) as *mut u8
        } else {
            // tc_realloc cannot preserve an over-aligned allocation's alignment,
            // so allocate fresh, copy, and free the old block by hand.
            let new_layout = Layout::from_size_align_unchecked(new_size, layout.align());
            let new_ptr = self.alloc(new_layout);
            if !new_ptr.is_null() {
                let copy = core::cmp::min(layout.size(), new_size);
                core::ptr::copy_nonoverlapping(ptr, new_ptr, copy);
                self.dealloc(ptr, layout);
            }
            new_ptr
        }
    }
}
