use super::val::{Val, ValType, TAG_STATUS};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Status(Val);

const fn decompose_status(body: u64) -> (u32, u32) {
    ((body as u32) >> 4, (body >> 32) as u32)
}

const fn compose_status(ty: u32, code: u32) -> u64 {
    (ty << 4) as u64 | (code as u64) << 32
}

const SST_OK: u32 = 0;
const SST_UNKNOWN: u32 = 1;
const SST_WASM_TRAP_CODE: u32 = 2;
const SST_HOST_TRAP_CODE: u32 = 3;
const SST_PAYMENT_RESULT: u32 = 4;
const SST_INVOKE_CONTRACT_RESULT: u32 = 5;
// See SCStatusType in stellar-transaction.x

pub const UNKNOWN_ERROR: Status =
    Status(unsafe { Val::from_body_and_tag(compose_status(SST_UNKNOWN, 0), TAG_STATUS) });

impl ValType for Status {
    #[inline(always)]
    fn is_val_type(v: Val) -> bool {
        v.has_tag(TAG_STATUS)
    }

    #[inline(always)]
    unsafe fn unchecked_from_val(v: Val) -> Self {
        Status(v)
    }
}

impl From<Status> for Val {
    #[inline(always)]
    fn from(s: Status) -> Self {
        s.0
    }
}

impl Status {
    fn is_type(&self, ty: u32) -> bool {
        let (t, _) = decompose_status(self.0.get_body());
        ty == t
    }
    pub fn is_ok(&self) -> bool {
        self.is_type(SST_OK)
    }
    pub fn is_wasm_trap(&self) -> bool {
        self.is_type(SST_WASM_TRAP_CODE)
    }
    pub fn is_host_trap(&self) -> bool {
        self.is_type(SST_HOST_TRAP_CODE)
    }
}
