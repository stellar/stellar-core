// This code is adapted from https://github.com/wenyuzhao/bump-allocator-rs

use core::alloc::{Layout, GlobalAlloc};

pub static mut LOCAL_ALLOCATOR: BumpPointerLocal = BumpPointerLocal::new();

pub struct BumpPointerLocal {
    cursor: *mut u8,
    limit: *mut u8,
}

impl BumpPointerLocal {
    const LOG_PAGE_SIZE: usize = 16;
    const PAGE_SIZE: usize = 1 << Self::LOG_PAGE_SIZE; // 64KB

    pub const fn new() -> Self {
        Self { cursor: 0 as _, limit: 0 as _ }
    }

    #[inline(always)]
    fn align_allocation(cursor: *mut u8, align: usize) -> *mut u8 {
        let mask = align - 1;
        (((cursor as usize) + mask) & !mask) as _
    }

    #[inline(always)]
    pub fn alloc(&mut self, bytes: usize, align: usize) -> *mut u8 {
        let start = Self::align_allocation(self.cursor, align);
        let new_cursor = unsafe { start.add(bytes) };
        if new_cursor <= self.limit {
            self.cursor = new_cursor;
            start
        } else {
            self.alloc_slow(bytes, align)
        }
    }

    #[inline(always)]
    fn alloc_slow_inline(&mut self, bytes: usize, align: usize) -> *mut u8 {
        let pages = (bytes + Self::PAGE_SIZE - 1) / Self::PAGE_SIZE;
        if core::arch::wasm32::memory_grow(0, pages) == usize::MAX {
            super::rt::trap()
        }
        self.limit = unsafe { self.cursor.add(pages * Self::PAGE_SIZE) };
        self.alloc(bytes, align)
    }

    #[inline(never)]
    fn alloc_slow(&mut self, bytes: usize, align: usize) -> *mut u8 {
        self.alloc_slow_inline(bytes, align)
    }
}


pub struct BumpPointer;

unsafe impl GlobalAlloc for BumpPointer {
    #[inline(always)]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let (bytes, align) = (layout.size(), layout.align());
        let ptr = LOCAL_ALLOCATOR.alloc(bytes, align);
        ptr
    }

    #[inline(always)]
    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: Layout) {}
}

#[global_allocator]
static GLOBAL: BumpPointer = BumpPointer;
