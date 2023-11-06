#![no_std]
use soroban_sdk::{contract, contractimpl, Env, Bytes, BytesN};

pub(crate) const EXTEND_AMOUNT: u32 = 518400; // 30 days
pub(crate) const EXTEND_THRESHOLD: u32 = EXTEND_AMOUNT;

#[contract]
pub struct WriteBytesContract;

#[contractimpl]
impl WriteBytesContract {
    pub fn write(env: Env, xdr_bytes: Bytes) -> BytesN<32> {
        let hash = env.crypto().sha256(&xdr_bytes);
        env.storage().temporary().set(&hash, &xdr_bytes);
        env.storage().temporary().extend(&hash, EXTEND_THRESHOLD, EXTEND_AMOUNT);

        env.storage().instance().extend(EXTEND_THRESHOLD, EXTEND_AMOUNT);

        hash
    }
}

mod test;
