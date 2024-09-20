#include "main/SettingsUpgradeUtils.h"
#include "crypto/SHA.h"
#include "rust/RustBridge.h"
#include "transactions/TransactionUtils.h"
#include <xdrpp/autocheck.h>

namespace stellar
{

std::pair<TransactionEnvelope, LedgerKey>
getWasmRestoreTx(PublicKey const& publicKey, SequenceNumber seqNum)
{
    TransactionEnvelope txEnv;
    txEnv.type(ENVELOPE_TYPE_TX);

    auto& tx = txEnv.v1().tx;
    tx.sourceAccount = toMuxedAccount(publicKey);
    tx.fee = 100'000'000;
    tx.seqNum = seqNum;

    Preconditions cond;
    cond.type(PRECOND_NONE);
    tx.cond = cond;

    Memo memo;
    memo.type(MEMO_NONE);
    tx.memo = memo;

    Operation restoreOp;
    restoreOp.body.type(RESTORE_FOOTPRINT);

    tx.operations.emplace_back(restoreOp);

    auto const writeByteWasm = rust_bridge::get_write_bytes();
    std::vector<uint8_t> wasm(writeByteWasm.data.begin(),
                              writeByteWasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasm);

    SorobanResources restoreResources;
    restoreResources.footprint.readWrite = {contractCodeLedgerKey};
    restoreResources.instructions = 0;
    restoreResources.readBytes = 2000;
    restoreResources.writeBytes = 2000;

    tx.ext.v(1);
    tx.ext.sorobanData().resources = restoreResources;
    tx.ext.sorobanData().resourceFee = 55'000'000;

    return {txEnv, contractCodeLedgerKey};
}

std::pair<TransactionEnvelope, LedgerKey>
getUploadTx(PublicKey const& publicKey, SequenceNumber seqNum)
{
    TransactionEnvelope txEnv;
    txEnv.type(ENVELOPE_TYPE_TX);

    auto& tx = txEnv.v1().tx;
    tx.sourceAccount = toMuxedAccount(publicKey);
    tx.fee = 100'000'000;
    tx.seqNum = seqNum;

    Preconditions cond;
    cond.type(PRECOND_NONE);
    tx.cond = cond;

    Memo memo;
    memo.type(MEMO_NONE);
    tx.memo = memo;

    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);

    auto const writeByteWasm = rust_bridge::get_write_bytes();
    uploadHF.wasm().assign(writeByteWasm.data.begin(),
                           writeByteWasm.data.end());

    tx.operations.emplace_back(uploadOp);

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(uploadHF.wasm());

    SorobanResources uploadResources;
    uploadResources.footprint.readWrite = {contractCodeLedgerKey};
    uploadResources.instructions = 2'000'000;
    uploadResources.readBytes = 2000;
    uploadResources.writeBytes = 2000;

    tx.ext.v(1);
    tx.ext.sorobanData().resources = uploadResources;
    tx.ext.sorobanData().resourceFee = 55'000'000;

    return {txEnv, contractCodeLedgerKey};
}

std::tuple<TransactionEnvelope, LedgerKey, Hash>
getCreateTx(PublicKey const& publicKey, LedgerKey const& contractCodeLedgerKey,
            std::string const& networkPassphrase, SequenceNumber seqNum)
{
    TransactionEnvelope txEnv;
    txEnv.type(ENVELOPE_TYPE_TX);

    auto& tx = txEnv.v1().tx;
    tx.sourceAccount = toMuxedAccount(publicKey);
    tx.fee = 25'000'000;
    tx.seqNum = seqNum;

    Preconditions cond;
    cond.type(PRECOND_NONE);
    tx.cond = cond;

    Memo memo;
    memo.type(MEMO_NONE);
    tx.memo = memo;

    ContractIDPreimage idPreimage(CONTRACT_ID_PREIMAGE_FROM_ADDRESS);
    idPreimage.fromAddress().address.type(SC_ADDRESS_TYPE_ACCOUNT);
    idPreimage.fromAddress().address.accountId().ed25519() =
        publicKey.ed25519();
    idPreimage.fromAddress().salt = autocheck::generator<Hash>()(5);

    HashIDPreimage fullPreImage;
    fullPreImage.type(ENVELOPE_TYPE_CONTRACT_ID);
    fullPreImage.contractID().contractIDPreimage = idPreimage;
    fullPreImage.contractID().networkID = sha256(networkPassphrase);

    auto contractID = xdrSha256(fullPreImage);

    Operation createOp;
    createOp.body.type(INVOKE_HOST_FUNCTION);
    auto& createHF = createOp.body.invokeHostFunctionOp().hostFunction;
    createHF.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);
    auto& createContractArgs = createHF.createContract();
    createContractArgs.contractIDPreimage = idPreimage;
    createContractArgs.executable.type(CONTRACT_EXECUTABLE_WASM);
    createContractArgs.executable.wasm_hash() =
        contractCodeLedgerKey.contractCode().hash;

    SorobanAuthorizationEntry auth;
    auth.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    auth.rootInvocation.function.type(
        SOROBAN_AUTHORIZED_FUNCTION_TYPE_CREATE_CONTRACT_HOST_FN);
    auth.rootInvocation.function.createContractHostFn().contractIDPreimage =
        idPreimage;
    auth.rootInvocation.function.createContractHostFn().executable.type(
        CONTRACT_EXECUTABLE_WASM);
    auth.rootInvocation.function.createContractHostFn().executable.wasm_hash() =
        contractCodeLedgerKey.contractCode().hash;
    createOp.body.invokeHostFunctionOp().auth = {auth};

    tx.operations.emplace_back(createOp);

    // Build footprint
    SCVal scContractSourceRefKey(SCValType::SCV_LEDGER_KEY_CONTRACT_INSTANCE);

    LedgerKey contractSourceRefLedgerKey;
    contractSourceRefLedgerKey.type(CONTRACT_DATA);
    contractSourceRefLedgerKey.contractData().contract.type(
        SC_ADDRESS_TYPE_CONTRACT);
    contractSourceRefLedgerKey.contractData().contract.contractId() =
        contractID;
    contractSourceRefLedgerKey.contractData().key = scContractSourceRefKey;
    contractSourceRefLedgerKey.contractData().durability =
        ContractDataDurability::PERSISTENT;

    SorobanResources uploadResources;
    uploadResources.footprint.readOnly = {contractCodeLedgerKey};
    uploadResources.footprint.readWrite = {contractSourceRefLedgerKey};
    uploadResources.instructions = 2'000'000;
    uploadResources.readBytes = 2000;
    uploadResources.writeBytes = 120;

    tx.ext.v(1);
    tx.ext.sorobanData().resources = uploadResources;
    tx.ext.sorobanData().resourceFee = 15'000'000;

    return {txEnv, contractSourceRefLedgerKey, contractID};
}

void
validateConfigUpgradeSet(ConfigUpgradeSet const& upgradeSet)
{
    for (auto const& entry : upgradeSet.updatedEntry)
    {
        if (entry.configSettingID() == CONFIG_SETTING_CONTRACT_LEDGER_COST_V0)
        {
            if (entry.contractLedgerCost().bucketListWriteFeeGrowthFactor >
                50'000)
            {
                throw std::runtime_error("Invalid contractLedgerCost");
            }
        }
        else if (entry.configSettingID() == CONFIG_SETTING_STATE_ARCHIVAL)
        {
            // 1048576 is our current phase1 scan size, and we don't expect to
            // go past this anytime soon.
            if (entry.stateArchivalSettings().evictionScanSize > 1048576)
            {
                throw std::runtime_error("Invalid evictionScanSize");
            }
        }
    }
}

std::pair<TransactionEnvelope, ConfigUpgradeSetKey>
getInvokeTx(PublicKey const& publicKey, LedgerKey const& contractCodeLedgerKey,
            LedgerKey const& contractSourceRefLedgerKey, Hash const& contractID,
            ConfigUpgradeSet const& upgradeSet, SequenceNumber seqNum)
{

    validateConfigUpgradeSet(upgradeSet);

    TransactionEnvelope txEnv;
    txEnv.type(ENVELOPE_TYPE_TX);

    auto& tx = txEnv.v1().tx;
    tx.sourceAccount = toMuxedAccount(publicKey);
    tx.fee = 100'000'000;
    tx.seqNum = seqNum;

    Preconditions cond;
    cond.type(PRECOND_NONE);
    tx.cond = cond;

    Memo memo;
    memo.type(MEMO_NONE);
    tx.memo = memo;

    Operation invokeOp;
    invokeOp.body.type(INVOKE_HOST_FUNCTION);
    auto& invokeHF = invokeOp.body.invokeHostFunctionOp().hostFunction;
    invokeHF.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);

    SCAddress addr(SC_ADDRESS_TYPE_CONTRACT);
    addr.contractId() = contractID;
    invokeHF.invokeContract().contractAddress = addr;

    const std::string& functionNameStr = "write";
    SCSymbol functionName;
    functionName.assign(functionNameStr.begin(), functionNameStr.end());
    invokeHF.invokeContract().functionName = functionName;

    auto upgradeSetBytes(xdr::xdr_to_opaque(upgradeSet));
    SCVal b(SCV_BYTES);
    b.bytes() = upgradeSetBytes;
    invokeHF.invokeContract().args.emplace_back(b);

    tx.operations.emplace_back(invokeOp);

    LedgerKey upgrade(CONTRACT_DATA);
    upgrade.contractData().durability = TEMPORARY;
    upgrade.contractData().contract = addr;

    SCVal upgradeHashBytes(SCV_BYTES);
    auto upgradeHash = sha256(upgradeSetBytes);
    upgradeHashBytes.bytes() = xdr::xdr_to_opaque(upgradeHash);
    upgrade.contractData().key = upgradeHashBytes;

    SorobanResources invokeResources;
    invokeResources.footprint.readOnly = {contractSourceRefLedgerKey,
                                          contractCodeLedgerKey};
    invokeResources.footprint.readWrite = {upgrade};
    invokeResources.instructions = 2'000'000;
    invokeResources.readBytes = 3200;
    invokeResources.writeBytes = 3200;

    tx.ext.v(1);
    tx.ext.sorobanData().resources = invokeResources;
    tx.ext.sorobanData().resourceFee = 65'000'000;

    ConfigUpgradeSetKey key;
    key.contentHash = upgradeHash;
    key.contractID = contractID;

    return {txEnv, key};
}

}