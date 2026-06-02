tcmalloc
========

A drop-in [`GlobalAlloc`][1] implementation using `tcmalloc` from [gperftools][2].

[![Travis badge](https://travis-ci.org/jmcomets/tcmalloc-rs.svg?branch=master)](https://travis-ci.org/jmcomets/tcmalloc-rs)
[![crates.io badge](https://img.shields.io/crates/v/tcmalloc.svg)](https://crates.io/crates/tcmalloc)

# Usage

Requires Rust 1.28+

```rust
extern crate tcmalloc;

use tcmalloc::TCMalloc;

#[global_allocator]
static GLOBAL: TCMalloc = TCMalloc;
```

Also note that you can only define one *global* allocator per application.

By default this crate expects to link to a system-installed tcmalloc. To build
the bundled copy of tcmalloc, enable the "bundled" feature. Note the [caveats
about profiling and libunwind/libgcc on 64-bit linux][4] &mdash; they are not
built by this crate, and tcmalloc will silently link to versions of both that
can cause profiling deadlocks.



[1]: https://doc.rust-lang.org/nightly/core/alloc/trait.GlobalAlloc.html
[2]: https://github.com/gperftools/gperftools
[3]: https://doc.rust-lang.org/nightly/unstable-book/language-features/global-allocator.html
[4]: https://github.com/gperftools/gperftools/blob/master/INSTALL#L33
