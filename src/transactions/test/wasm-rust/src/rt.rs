// provide bits of rust's runtime interface: allocator, panic handling, etc.

#[cfg(target_arch = "x86_64")]
use core::arch::asm;

#[cfg(target_arch = "x86_64")]
use core::hint::unreachable_unchecked;

#[cfg(target_arch = "wasm32")]
#[inline(always)]
pub fn trap() -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(target_arch = "x86_64")]
pub fn trap() -> ! {
    unsafe {
        asm!("ud2");
        unreachable_unchecked()
    }
}

#[cfg(not(test))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    trap();
}
