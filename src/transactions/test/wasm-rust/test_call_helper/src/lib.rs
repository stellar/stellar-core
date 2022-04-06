#![no_std]
use sdk::Val;
use stellar_contract_sdk as sdk;

mod helper_contract_interface;
use helper_contract_interface::HelperContract;

// Here we're using the typesafe HelperContract interface
// to compose a call to another contract (whose LedgerKey
// identity is passed to us as an argument)

#[no_mangle]
pub fn call(contract: HelperContract) -> Val {
    contract.helper(10)
}
