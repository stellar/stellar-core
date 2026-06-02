// SPDX-License-Identifier: MIT
// Compatible with OpenZeppelin Stellar Soroban Contracts ^0.6.0
// Generated with OpenZeppelin Contracts Wizard.


use soroban_sdk::{
    Address, contract, contractimpl, Env, MuxedAddress, String, Vec
};
use stellar_access::ownable::{self as ownable, Ownable};
use stellar_contract_utils::pausable::{self as pausable, Pausable};
use stellar_contract_utils::upgradeable::UpgradeableInternal;
use stellar_macros::{only_owner, Upgradeable, when_not_paused};
use stellar_tokens::fungible::{Base, burnable::FungibleBurnable, FungibleToken};

#[derive(Upgradeable)]
#[contract]
pub struct ApplyLoadToken;

#[contractimpl]
impl ApplyLoadToken {
    pub fn __constructor(e: &Env, owner: Address) {
        Base::set_metadata(e, 7, String::from_str(e, "ApplyLoadToken"), String::from_str(e, "ALT"));
        ownable::set_owner(e, &owner);
    }

    #[only_owner]
    #[when_not_paused]
    pub fn multi_mint(e: &Env, accounts: Vec<Address>, amount: i128) {
        for account in accounts.iter() {
            Base::mint(e, &account, amount);
        }
    }
}

#[contractimpl(contracttrait)]
impl FungibleToken for ApplyLoadToken {
    type ContractType = Base;

    #[when_not_paused]
    fn transfer(e: &Env, from: Address, to: MuxedAddress, amount: i128) {
        Self::ContractType::transfer(e, &from, &to, amount);
    }

    #[when_not_paused]
    fn transfer_from(e: &Env, spender: Address, from: Address, to: Address, amount: i128) {
        Self::ContractType::transfer_from(e, &spender, &from, &to, amount);
    }
}

//
// Extensions
//

#[contractimpl(contracttrait)]
impl FungibleBurnable for ApplyLoadToken {
    #[when_not_paused]
    fn burn(e: &Env, from: Address, amount: i128) {
        Base::burn(e, &from, amount);
    }

    #[when_not_paused]
    fn burn_from(e: &Env, spender: Address, from: Address, amount: i128) {
        Base::burn_from(e, &spender, &from, amount);
    }
}

//
// Utils
//

impl UpgradeableInternal for ApplyLoadToken {
    fn _require_auth(e: &Env, _operator: &Address) {
        ownable::enforce_owner_auth(e);
    }
}

#[contractimpl(contracttrait)]
impl Ownable for ApplyLoadToken {}

#[contractimpl]
impl Pausable for ApplyLoadToken {
    fn paused(e: &Env) -> bool {
        pausable::paused(e)
    }

    #[only_owner]
    fn pause(e: &Env, _caller: Address) {
        pausable::pause(e);
    }

    #[only_owner]
    fn unpause(e: &Env, _caller: Address) {
        pausable::unpause(e);
    }
}
