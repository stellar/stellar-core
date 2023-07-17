#![no_std]
use soroban_sdk::{contract, contractimpl, contractmeta, Env, Bytes, BytesN};

pub(crate) const BUMP_AMOUNT: u32 = 518400; // 30 days

contractmeta!(
    key = "v",
    val = "1"
);

#[contract]
pub struct WriteBytesContract;

#[contractimpl]
impl WriteBytesContract {
    pub fn write(env: Env, xdr_bytes: Bytes) -> BytesN<32> {
        let hash = env.crypto().sha256(&xdr_bytes);
        env.storage().persistent().set(&hash, &xdr_bytes);
        env.storage().persistent().bump(&hash, BUMP_AMOUNT);

        env.storage().instance().bump(BUMP_AMOUNT);

        hash
    }

    pub fn get(env: Env, hash: BytesN<32>) -> Bytes {
        env.storage().persistent().get(&hash).unwrap()
    }
}

mod test;
