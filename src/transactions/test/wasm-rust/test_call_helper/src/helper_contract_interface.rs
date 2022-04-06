use sdk::{Symbol, Val};
use stellar_contract_sdk as sdk;

// This file contains a wrapper type around a Val that presents a static
// interface to the methods on the helper contract. It implements them by
// calling the sdk::callN methods with symbolic method names.
//
// It is assumed that this interface would likely be generated from a
// declarative interface-definition associated with a contract -- possibly the
// same definition used to generate a trait for the Helper contract to implement
// and an external client-SDK interface, for clients composing transactions.
//
// All of this is just for type-system pleasantry; it is inlined and optimized
// away at contract-compile time.

#[repr(transparent)]
pub struct HelperContract(Val);

impl HelperContract {
    #[inline(always)]
    pub fn helper(&self, v: u32) -> Val {
        const HELPER: Symbol = Symbol::from_str("helper");
        sdk::call1(self.0, HELPER, v.into())
    }
}
