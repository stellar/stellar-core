from stellar_sdk.xdr import *
from stellar_sdk import Network, Keypair, TransactionBuilder, StrKey, utils, scval
from stellar_sdk.exceptions import PrepareTransactionException
from stellar_sdk.soroban_server import SorobanServer
from stellar_sdk.soroban_rpc import GetTransactionStatus
from stellar_sdk.xdr import TransactionMeta, LedgerEntryType, LedgerKey, ConfigSettingContractComputeV0, ConfigUpgradeSet, ConfigSettingContractLedgerCostV0, ConfigSettingContractHistoricalDataV0, ConfigSettingContractMetaDataV0, ConfigSettingContractBandwidthV0, ConfigUpgradeSetKey, ConfigSettingEntry, StateExpirationSettings, ConfigSettingContractExecutionLanesV0, Uint32, Uint64, Int64, Hash, LedgerKeyConfigSetting, ConfigSettingID
import stellar_sdk
from enum import IntEnum
import urllib.parse
import argparse
import time
import sys

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

    max_contract_size = Uint32(65536) # 64 kb

    contract_size_upgrade_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES,
        contract_max_size_bytes = max_contract_size)   
        
    compute_settings = ConfigSettingContractComputeV0(ledger_max_instructions=Int64(1000000000),
                                                 tx_max_instructions=Int64(100000000),
                                                 fee_rate_per_instructions_increment=Int64(100),
                                                 tx_memory_limit=Uint32(41943040)) # 40 mb

    compute_upgrade_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_COMPUTE_V0,
        contract_compute = compute_settings)    
    
    contract_ledger_cost_settings = ConfigSettingContractLedgerCostV0(ledger_max_read_ledger_entries=Uint32(400),
                                                 ledger_max_read_bytes=Uint32(2048000),
                                                 ledger_max_write_ledger_entries=Uint32(200),
                                                 ledger_max_write_bytes=Uint32(1024000),
                                                 tx_max_read_ledger_entries=Uint32(30),
                                                 tx_max_read_bytes=Uint32(133120), # 130 kb
                                                 tx_max_write_ledger_entries=Uint32(20),
                                                 tx_max_write_bytes=Uint32(66560), # 65 kb 
                                                 fee_read_ledger_entry=Int64(20000),
                                                 fee_write_ledger_entry=Int64(30000),
                                                 fee_read1_kb=Int64(1000),
                                                 fee_write1_kb=Int64(1000),
                                                 bucket_list_size_bytes=Int64(32212254720), # 30 * 1024 * 1024 * 1024 for 30 GB
                                                 bucket_list_fee_rate_low=Int64(1000),
                                                 bucket_list_fee_rate_high=Int64(10000),
                                                 bucket_list_growth_factor=Uint32(1))

    contract_ledger_cost_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_LEDGER_COST_V0,
        contract_ledger_cost = contract_ledger_cost_settings)        

    contract_historical_data_settings = ConfigSettingContractHistoricalDataV0(fee_historical1_kb=Int64(5000))

    contract_historical_data_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0,
        contract_historical_data = contract_historical_data_settings)    

    contract_meta_data_settings = ConfigSettingContractMetaDataV0(tx_max_extended_meta_data_size_bytes=Uint32(2048), # 2 kb
                                                                        fee_extended_meta_data1_kb=Int64(300))  

    contract_meta_data_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_META_DATA_V0,
        contract_meta_data = contract_meta_data_settings)    

    contract_bandwidth_settings = ConfigSettingContractBandwidthV0(ledger_max_propagate_size_bytes=Uint32(1024000), # 100 kb
                                                                        tx_max_size_bytes=Uint32(71620), # 70 kb
                                                                        fee_propagate_data1_kb=Int64(2000))   

    contract_bandwidth_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_BANDWIDTH_V0,
        contract_bandwidth = contract_bandwidth_settings)    
    
    contract_data_entry_size = Uint32(65536) # 64 kb

    contract_data_entry_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES,
        contract_data_entry_size_bytes = contract_data_entry_size)   

    state_exp_settings = StateExpirationSettings(max_entry_expiration=Uint32(535680), # 31 days
                                                 min_temp_entry_expiration=Uint32(16),
                                                 min_persistent_entry_expiration=Uint32(120960), # 7 days
                                                 auto_bump_ledgers=Uint32(0),
                                                 persistent_rent_rate_denominator=Int64(252480), #InitialSorobanNetworkConfig
                                                 temp_rent_rate_denominator=Int64(2524800), #InitialSorobanNetworkConfig
                                                 max_entries_to_expire=Uint32(100), #InitialSorobanNetworkConfig
                                                 bucket_list_size_window_sample_size=Uint32(30), #InitialSorobanNetworkConfig
                                                 eviction_scan_size=Uint64(100000)) #InitialSorobanNetworkConfig

    state_exp_upgrade_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_STATE_EXPIRATION,
        state_expiration_settings = state_exp_settings)

    execution_lanes_setting = ConfigSettingContractExecutionLanesV0(ledger_max_tx_count=Uint32(30))   

    execution_lanes_entry = ConfigSettingEntry(
        ConfigSettingID.CONFIG_SETTING_CONTRACT_EXECUTION_LANES,
        contract_execution_lanes = execution_lanes_setting)        
    
    return ConfigUpgradeSet([contract_size_upgrade_entry, compute_upgrade_entry, contract_ledger_cost_entry, contract_historical_data_entry, contract_meta_data_entry, contract_bandwidth_entry, contract_data_entry_entry, state_exp_upgrade_entry, execution_lanes_entry])
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

    try:
        tx = soroban_server.prepare_transaction(tx)
    except PrepareTransactionException as e:
        print(f"Got exception: {e.simulate_transaction_response}")
        raise e

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

    assert get_transaction_data.status == GetTransactionStatus.SUCCESS
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
            address=kp.public_key,
        )
        .build()
    )

    try:
        tx = soroban_server.prepare_transaction(tx)
    except PrepareTransactionException as e:
        print(f"Got exception: {e.simulate_transaction_response}")
        raise e

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
    assert get_transaction_data.result_meta_xdr is not None
    transaction_meta = TransactionMeta.from_xdr(
        get_transaction_data.result_meta_xdr
    )
    return transaction_meta.v3.soroban_meta.return_value.address.contract_id.hash  # type: ignore


def upload_upgrade_bytes(contract_id, upgrade):
    source = soroban_server.load_account(kp.public_key)

    tx = (
        TransactionBuilder(source, network_passphrase, base_fee=100)
        .add_time_bounds(0, 0)
        .append_invoke_contract_function_op(
            contract_id=contract_id,
            function_name="write",
            parameters=[scval.to_bytes(upgrade.to_xdr_bytes())],
        )
        .build()
    )

    try:
        tx = soroban_server.prepare_transaction(tx)
    except PrepareTransactionException as e:
        print(f"Got exception: {e.simulate_transaction_response}")
        raise e

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
    config_setting = LedgerKeyConfigSetting(
        config_setting_id=ConfigSettingID(int(args.configSettingID)))
    ledger_key = LedgerKey(
        LedgerEntryType.CONFIG_SETTING, 
        config_setting = config_setting)

    resp = soroban_server.get_ledger_entries([ledger_key])
    if resp.entries is None:
        raise RequestException(
            404,
            f"Ledger entry not found, maybe you need to activate",
        )
    assert len(resp.entries) == 1
    data = LedgerEntryData.from_xdr(resp.entries[0].xdr)
    assert data.config_setting is not None
    print(data)


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
