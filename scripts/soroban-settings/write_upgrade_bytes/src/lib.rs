#![no_std]
use soroban_sdk::{contract, contractimpl, Env, Bytes, BytesN};

pub(crate) const BUMP_AMOUNT: u32 = 518400; // 30 days
pub(crate) const EXPIRATION_LOW_WATERMARK: u32 = BUMP_AMOUNT;

#[contract]
pub struct WriteBytesContract;

#[contractimpl]
impl WriteBytesContract {
    pub fn write(env: Env, xdr_bytes: Bytes) -> BytesN<32> {
        let hash = env.crypto().sha256(&xdr_bytes);
        env.storage().temporary().set(&hash, &xdr_bytes);
        env.storage().temporary().bump(&hash, EXPIRATION_LOW_WATERMARK, BUMP_AMOUNT);

        env.storage().instance().bump(EXPIRATION_LOW_WATERMARK, BUMP_AMOUNT);

        hash
    }
}

mod test;
