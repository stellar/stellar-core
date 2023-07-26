from stellar_sdk import xdr as stellar_xdr
from stellar_sdk import Network, Keypair, TransactionBuilder, StrKey, utils
from stellar_sdk.soroban import SorobanServer
from stellar_sdk.soroban.types import Address, Int128, Bytes
from stellar_sdk.soroban.soroban_rpc import GetTransactionStatus
from stellar_sdk.xdr import TransactionMeta, LedgerKey, ConfigUpgradeSet, ConfigSettingContractExecutionLanesV0, ConfigUpgradeSetKey, ConfigSettingEntry, StateExpirationSettings, Uint32, Uint64, Int64, Hash, LedgerKeyConfigSetting, ConfigSettingID
import stellar_sdk
from enum import IntEnum
import urllib.parse
import argparse
import time
import sys

# The soroban branch of py-stellar-base hasn't been merged into main and released yet, so we have to install it locally.
sys.path.append("/Users/dev/py-stellar-base")

secret = "SAAPYAPTTRZMCUZFPG3G66V4ZMHTK4TWA6NS7U4F7Z3IMUD52EK4DDEV"
# public -> GDAT5HWTGIU4TSSZ4752OUC4SABDLTLZFRPZUJ3D6LKBNEPA7V2CIG54

# For standalone
# Fund standalone with friendbot - http://localhost:8000/friendbot?addr=GDAT5HWTGIU4TSSZ4752OUC4SABDLTLZFRPZUJ3D6LKBNEPA7V2CIG54
# rpc_server_url = "http://127.0.0.1:8000/soroban/rpc"
# network_passphrase = Network.STANDALONE_NETWORK_PASSPHRASE

# For futurenet
# Fund futurenet with friendbot - https://friendbot-futurenet.stellar.org/?addr=GDAT5HWTGIU4TSSZ4752OUC4SABDLTLZFRPZUJ3D6LKBNEPA7V2CIG54
rpc_server_url = "https://rpc-futurenet.stellar.org:443/"
network_passphrase = Network.FUTURENET_NETWORK_PASSPHRASE

contract_file_path = "write_upgrade_bytes/target/wasm32-unknown-unknown/release/soroban_write_upgrade_bytes_contract.wasm"

kp = Keypair.from_secret(secret)
soroban_server = SorobanServer(rpc_server_url)

# hardcode the upgrade you want to do here.
def get_upgrade_set():
    state_exp_settings = StateExpirationSettings(max_entry_expiration=Uint32(6312000),
                                                 min_temp_entry_expiration=Uint32(
                                                     16),
                                                 min_persistent_entry_expiration=Uint32(
                                                     86400),
                                                 auto_bump_ledgers=Uint32(0),
                                                 persistent_rent_rate_denominator=Int64(
                                                     0),
                                                 temp_rent_rate_denominator=Int64(
                                                     0),
                                                 max_entries_to_expire=Uint32(
                                                     0),
                                                 bucket_list_size_window_sample_size=Uint32(
                                                     0),
                                                 eviction_scan_size=Uint64(0))

    entry = ConfigSettingEntry.from_config_setting_state_expiration(
        state_exp_settings)
    return ConfigUpgradeSet([entry])
#############

# TODO: Update tx submissions to go directly to tx endpoint instead of rpc
# Upload and create contract


def deploy_contract():
    print("uploading contract...")
    source = soroban_server.load_account(kp.public_key)
    tx = (
        TransactionBuilder(source, network_passphrase)
        .set_timeout(300)
        .append_upload_contract_wasm_op(
            contract=contract_file_path,  # the path to the contract, or binary data
        )
        .build()
    )

    tx = soroban_server.prepare_transaction(tx)
    tx.sign(kp)
    send_transaction_data = soroban_server.send_transaction(tx)
    print(f"sent transaction: {send_transaction_data}")

    while True:
        print("waiting for transaction to be confirmed...")
        get_transaction_data = soroban_server.get_transaction(
            send_transaction_data.hash)
        if get_transaction_data.status != GetTransactionStatus.NOT_FOUND:
            break
        time.sleep(3)

    print(f"transaction: {get_transaction_data}\n\n")

    wasm_id = None
    if get_transaction_data.status == GetTransactionStatus.SUCCESS:
        assert get_transaction_data.result_meta_xdr is not None
        transaction_meta = TransactionMeta.from_xdr(
            get_transaction_data.result_meta_xdr
        )
        wasm_id = transaction_meta.v3.soroban_meta.return_value.bytes.sc_bytes.hex()  # type: ignore
        print(f"wasm id: {wasm_id}")

        assert wasm_id, "wasm id should not be empty"

    print("creating contract...")

    source = soroban_server.load_account(
        kp.public_key
    )  # refresh source account, because the current SDK will increment the sequence number by one after building a transaction

    tx = (
        TransactionBuilder(source, network_passphrase)
        .set_timeout(300)
        .append_create_contract_op(
            wasm_id=wasm_id,
        )
        .build()
    )

    tx = soroban_server.prepare_transaction(tx)
    tx.sign(kp)

    send_transaction_data = soroban_server.send_transaction(tx)
    print(f"sent transaction: {send_transaction_data}")

    while True:
        print("waiting for transaction to be confirmed...")
        get_transaction_data = soroban_server.get_transaction(
            send_transaction_data.hash)
        if get_transaction_data.status != GetTransactionStatus.NOT_FOUND:
            break
        time.sleep(3)

    print(f"transaction: {get_transaction_data}\n\n")

    if get_transaction_data.status == GetTransactionStatus.SUCCESS:
        assert get_transaction_data.result_meta_xdr is not None
        transaction_meta = stellar_xdr.TransactionMeta.from_xdr(
            get_transaction_data.result_meta_xdr
        )
        result = transaction_meta.v3.soroban_meta.return_value.address.contract_id.hash  # type: ignore
    return result


def upload_upgrade_bytes(contract_id, upgrade):
    source = soroban_server.load_account(kp.public_key)

    tx = (
        TransactionBuilder(source, network_passphrase, base_fee=100)
        .add_time_bounds(0, 0)
        .append_invoke_contract_function_op(
            contract_id=contract_id,
            function_name="write",
            parameters=[Bytes(upgrade.to_xdr_bytes())],
        )
        .build()
    )

    tx = soroban_server.prepare_transaction(tx)
    tx.sign(kp)
    send_transaction_data = soroban_server.send_transaction(tx)
    print(f"sent transaction: {send_transaction_data}")

    while True:
        print("waiting for transaction to be confirmed...")
        get_transaction_data = soroban_server.get_transaction(
            send_transaction_data.hash)
        if get_transaction_data.status != GetTransactionStatus.NOT_FOUND:
            break
        time.sleep(3)

    print(f"transaction: {get_transaction_data}\n\n")

    assert get_transaction_data.status == GetTransactionStatus.SUCCESS


def get_upgrade_key(contract_id, upgrade_hash):
    key = ConfigUpgradeSetKey(contract_id=Hash(
        contract_id), content_hash=Hash(upgrade_hash))
    return key


"""
The setup_upgrade mode will setup the ContractData required for the
upgrade, and then print out the url encoded xdr to submit to core
using the upgrades endpoint (upgrades?mode=set&configupgradesetkey=********).

Note that the upgrade is specified in code at the moment (upgrade = get_upgrade_set())
"""


def setup_upgrade(args):
    # Get upgrade xdr

    upgrade = get_upgrade_set()
    upgrade_xdr = upgrade.to_xdr()
    upgrade_hash = utils.sha256(upgrade.to_xdr_bytes())

    # Deploy contract
    contract_id_hash = deploy_contract()
    # print(f"contract id xdr: {Hash(contract_id_hash).to_xdr()}")
    print(f"contract id hex: {Hash(contract_id_hash).to_xdr_bytes().hex()}")

    contract_id_str = StrKey.encode_contract(contract_id_hash)
    print(f"contract id: {contract_id_str}")

    # Invoke contract
    upload_upgrade_bytes(contract_id_str, upgrade)

    # Get upgrade
    upgrade_key = get_upgrade_key(contract_id_hash, upgrade_hash).to_xdr()
    url_encoded_key = urllib.parse.quote(upgrade_key)
    print(f"url encoded upgrade: {url_encoded_key}")


def get_settings(args):
    setting_key = LedgerKeyConfigSetting(
        config_setting_id=ConfigSettingID(int(args.configSettingID)))
    ledger_key = LedgerKey.from_config_setting(setting_key)

    resp = soroban_server.get_ledger_entries([ledger_key])
    if resp.entries is None:
        raise RequestException(
            404,
            f"Ledger entry not found, maybe you need to activate",
        )
    assert len(resp.entries) == 1
    data = stellar_xdr.LedgerEntryData.from_xdr(resp.entries[0].xdr)
    assert data.config_setting is not None
    print(data)


def get_ledger_key() -> str:
    setting_key = LedgerKeyConfigSetting(
        config_setting_id=ConfigSettingID.CONFIG_SETTING_STATE_EXPIRATION)
    ledger_key = LedgerKey.from_config_setting(setting_key)
    return ledger_key


def main():
    argument_parser = argparse.ArgumentParser()
    subparsers = argument_parser.add_subparsers(required=True)

    parser_setup_upgrade = subparsers.add_parser(
        "setupUpgrade",
        help="Get upgrade for hardcoded settings")
    parser_setup_upgrade.set_defaults(func=setup_upgrade)

    parser_get_settings = subparsers.add_parser(
        "getSettings",
        help="Get settings")
    parser_get_settings.add_argument("-id",
                                     "--configSettingID",
                                     required=True,
                                     help="The integer ConfigSettingID value being queried")
    parser_get_settings.set_defaults(func=get_settings)

    args = argument_parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
