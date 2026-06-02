#![cfg(test)]

extern crate std;

use soroban_sdk::{
    symbol_short,
    testutils::{Address as _, AuthorizedFunction, AuthorizedInvocation},
    vec, Address, Env, IntoVal, MuxedAddress, Symbol,
};

use crate::contract::{ApplyLoadToken, ApplyLoadTokenClient};

#[test]
fn test_multi_mint_and_transfer() {
    let env = Env::default();
    env.mock_all_auths();

    let owner = Address::generate(&env);
    let alice = Address::generate(&env);
    let bob = Address::generate(&env);

    let contract_addr = env.register(ApplyLoadToken, (owner.clone(),));
    let client = ApplyLoadTokenClient::new(&env, &contract_addr);

    let recipients = vec![&env, alice.clone(), bob.clone()];
    client.multi_mint(&recipients, &100);

    assert_eq!(
        env.auths(),
        std::vec![(
            owner.clone(),
            AuthorizedInvocation {
                function: AuthorizedFunction::Contract((
                    client.address.clone(),
                    Symbol::new(&env, "multi_mint"),
                    (recipients.clone(), 100_i128).into_val(&env),
                )),
                sub_invocations: std::vec![],
            },
        )]
    );

    assert_eq!(client.balance(&alice), 100);
    assert_eq!(client.balance(&bob), 100);

    client.transfer(&alice, &MuxedAddress::from(bob.clone()), &40);

    assert_eq!(
        env.auths(),
        std::vec![(
            alice.clone(),
            AuthorizedInvocation {
                function: AuthorizedFunction::Contract((
                    client.address.clone(),
                    symbol_short!("transfer"),
                    (alice.clone(), MuxedAddress::from(bob.clone()), 40_i128,).into_val(&env),
                )),
                sub_invocations: std::vec![],
            },
        )]
    );

    assert_eq!(client.balance(&alice), 60);
    assert_eq!(client.balance(&bob), 140);
}
