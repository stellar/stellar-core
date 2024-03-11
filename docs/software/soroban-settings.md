---
title: Soroban Settings
---

Soroban has a large set of settings stored on ledger that can be modified
through a validator vote. This document describes how to propose a new settings
upgrade, and how to examine a proposed upgrade. You can also look at the
[commands.md](commands.md) doc for more details on the stellar-core commands used below.

## Propose a settings upgrade

This section will describe how to propose a settings upgrade, but take a look at
the [admin page](admin.md#upgrading-soroban-settings) for more information on
how the settings upgrade mechanism works internally. **If you are just being asked
to vote for an upgrade, please move on to the [Examine a proposed upgrade](#examine-a-proposed-upgrade) step for details on how to examine a
proposed upgrade.**

1. Using the stellar-xdr rust tool and a JSON file as input, run `stellar-xdr encode --type ConfigUpgradeSet JSON_FILE`. [pubnet_phase1.json](../../soroban-settings/pubnet_phase1.json) can be used as a template to propose upgrades.
    - Example output - `AAAAAQAAAAsAAAAM` (note that this string can be much larger depending on the settings you're upgrading)
    - stellar-xdr can be installed with brew (`brew install stellar/tap/stellar-xdr`) or cargo (``cargo install --locked stellar-xdr --features cli``). You can also download the binary for your system here https://github.com/stellar/rs-stellar-xdr/releases/latest.

2. Using the output from the command above, run the `stellar-core get-settings-upgrade-txs PUBLIC_KEY SEQ_NUM NETWORK_PASSPHRASE --xdr CONFIG_UPGRADE_SET_XDR --signtxs` command. Note that you will be submitting transactions, so the account for the public key specified must exist and have the funds to pay the transaction fees.
    - Example command - `stellar-core get-settings-upgrade-txs GAUQW73V52I2WLIPKCKYXZBHIYFTECS7UPSG4OSVUHNDXEZJJWFXZG56 73014444032 "Public Global Stellar Network ; September 2015" --xdr AAAAAQAAAAsAAAAM --signtxs`
    - You will be prompted for the secret key that corresponds to the public key passed in. Provide it to sign the transactions required to set up the upgrade.
    - Sample output with truncated `TransactionEnvelopes`
        - AAAAAgAAAABi/B0L0JGythwN1lY0aypo19NHxvLCyO5tBEc...wF9wL68IAAAAdkJxSgpyRStTvbSA9jgs=
        - 19c49f18e5442db9d626f7485c34ecb0cd938034255515099b37acebdb6677a7
        - AAAAAgAAAABi/B0L0JGythwN1lY0aypo19NHxvLCyO5tBEc...AAd0OB3n3Yadews=
        - 9e70cbff631247638fae96b9d996d8d22b6fa75208380d5f5d714a57c0a90947
        - AAAAAgAAAABi/B0L0JGythwN1lY0aypo19NHxvLCyO5tBEc...F9wX14QAAAAARAAAAAwAAAAAAAAAAAAAAAQAAAAAAAAAYAAAAAAAAAAGd/IhWQcE2UdzIof7ygqCuAmYD8ycsJbB
        - 4f6457d3fc081ab3a72dfe7ee2236f3a282c62f21d7e3dbdb5b13ac0d09c8647
        - nfyIVkHBNlHcyKH+8oKgrgJmA/MnLCW3E4Fhg4XYTkqZa2MyqzRdB2+mN3DOKUFKtZIAXp6o3DHrkgR0mo7rUw==
    - This is the format of the output above - <br> 
        &ensp;1.Base64 upload `TransactionEnvelope` XDR<br>
        &ensp;2.Hex tx ID for the upload tx<br>
        &ensp;3.Base 64 create `TransactionEnvelope` XDR<br>
        &ensp;4.Hex tx ID for the create tx<br>
        &ensp;5.Base64 invoke `TransactionEnvelope` XDR<br>
        &ensp;6.Hex tx ID for the invoke tx<br>
        &ensp;7.Base64 `ConfigUpgradeSetKey` XDR<br>
3. Submit three transactions to the network to set up the upgrade, replacing the blob placeholders below with the `TransactionEnvelopes` in lines 1, 3, and 5.
    1. `curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=<LINE_1_OUTPUT>'`
    2. `curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=<LINE_3_OUTPUT>'`
    3. `curl -G 'http://localhost:11626/tx' --data-urlencode 'blob=<LINE_5_OUTPUT>'`
4. You can verify that the proposed upgrades have been set up using the `dumpproposedsettings` and the output from line 7 (the `ConfigUpgradeSetKey`)
    1. `curl -G 'http://localhost:11626/dumpproposedsettings' --data-urlencode 'blob=<LINE_7_OUTPUT>'`
5. Now schedule the upgrade on all the required validators using the output from line 7 (the `ConfigUpgradeSetKey`) and an agreed upon time in the future
    1. `curl -G 'http://localhost:11626/upgrades?mode=set&upgradetime=YYYY-MM-DDTHH:MM:SSZ' --data-urlencode 'configupgradesetkey=<LINE_7_OUTPUT>'`
6. Update https://github.com/stellar-expert/staged-soroban-upgrades so https://stellar.expert/explorer/pubnet/protocol-history will show the proposed upgrade.

### Helper script
A script to help with crafting the transactions above is available [here](../../scripts/settings-helper.sh) with usage details in [README.md](../../scripts/README.md), but it's important to be aware of how the underlying process works in case the script has some issues.

### Debugging
If any of the transactions above fail during transaction submission, you should get a `TransactionResult` as a response with the reason.

Once the three transactions are run, you should see the proposed upgrade when running the `dumpproposedsettings` command [(step 4)](#propose-a-settings-upgrade). If you don't, then either one or more of the transactions above failed during application, or the upgrade is invalid.

If any of the transactions above fail during transaction application, the failure will most likely be due to one of the following, and you should confirm this by looking at the `TransactionResult` of the failed transaction using the Stellar Laboratory or an explorer - 
1. Resources are too low. You'll need to increase the hardcoded resources in [SettingsUpgradeUtils.cpp](../../src/main/SettingsUpgradeUtils.cpp).
2. Fee or refundable fee is too low. You'll need to increase them in [SettingsUpgradeUtils.cpp](../../src/main/SettingsUpgradeUtils.cpp).
3. Wasm has expired. You'll need to restore the Wasm.

If the transactions succeeded but the `dumpproposedsettings` command still returns an error, then the upgrade is invalid. The error reporting here needs to be improved, but the validity checks happen [here](../../src/herder/Upgrades.cpp).

## Examine a proposed upgrade

You can use the `dumpproposedsettings` http command along with a base64 encoded XDR
serialized `ConfigUpgradeSetKey` to query a proposed upgrade. 
Example - `curl -G 'http://localhost:11626/dumpproposedsettings' --data-urlencode 'blob=A6MvjFLujnqaZa5hacafWyYwhpk4cgRpyu0z6ilZ0pm1S7fmjSNnsyjGwGodLGiD8ss8S1AHiOBBb6GQbOeMbw=='`

You can also get the current Soroban settings to compare against by using the command under the [Examine current settings](#examine-current-settings) section.

## Examine current settings

Examine the current settings with `curl -G 'http://localhost:11626/sorobaninfo' --data-urlencode 'format=detailed'`.