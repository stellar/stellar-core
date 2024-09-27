#include "simulation/ApplyLoad.h"

#include <numeric>

#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "test/TxTests.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"

#include "herder/HerderImpl.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "util/XDRCereal.h"
#include <crypto/SHA.h>

namespace stellar
{

ApplyLoad::ApplyLoad(Application& app, uint64_t ledgerMaxInstructions,
                     uint64_t ledgerMaxReadLedgerEntries,
                     uint64_t ledgerMaxReadBytes,
                     uint64_t ledgerMaxWriteLedgerEntries,
                     uint64_t ledgerMaxWriteBytes, uint64_t ledgerMaxTxCount,
                     uint64_t ledgerMaxTransactionsSizeBytes)
    : mTxGenerator(app)
    , mApp(app)
    , mNumAccounts(
          ledgerMaxTxCount * SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER + 1)
    , mTxCountUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "tx-count"}))
    , mInstructionUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "ins"}))
    , mTxSizeUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "tx-size"}))
    , mReadByteUtilization(
          mApp.getMetrics().NewHistogram({"soroban", "benchmark", "read-byte"}))
    , mWriteByteUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "benchmark", "write-byte"}))
    , mReadEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "benchmark", "read-entry"}))
    , mWriteEntryUtilization(mApp.getMetrics().NewHistogram(
          {"soroban", "benchmark", "write-entry"}))
{

    auto rootTestAccount = TestAccount::createRoot(mApp);
    mRoot = std::make_shared<TestAccount>(rootTestAccount);
    releaseAssert(mTxGenerator.loadAccount(mRoot));

    mUpgradeConfig.maxContractSizeBytes = 65536;
    mUpgradeConfig.maxContractDataKeySizeBytes = 250;
    mUpgradeConfig.maxContractDataEntrySizeBytes = 65536;
    mUpgradeConfig.ledgerMaxInstructions = ledgerMaxInstructions;
    mUpgradeConfig.txMaxInstructions = 100000000;
    mUpgradeConfig.txMemoryLimit = 41943040;
    mUpgradeConfig.ledgerMaxReadLedgerEntries = ledgerMaxReadLedgerEntries;
    mUpgradeConfig.ledgerMaxReadBytes = ledgerMaxReadBytes;
    mUpgradeConfig.ledgerMaxWriteLedgerEntries = ledgerMaxWriteLedgerEntries;
    mUpgradeConfig.ledgerMaxWriteBytes = ledgerMaxWriteBytes;
    mUpgradeConfig.ledgerMaxTxCount = ledgerMaxTxCount;
    mUpgradeConfig.txMaxReadLedgerEntries = 40;
    mUpgradeConfig.txMaxReadBytes = 200000;
    mUpgradeConfig.txMaxWriteLedgerEntries = 25;
    mUpgradeConfig.txMaxWriteBytes = 66560;
    mUpgradeConfig.txMaxContractEventsSizeBytes = 8198;
    mUpgradeConfig.ledgerMaxTransactionsSizeBytes =
        ledgerMaxTransactionsSizeBytes;
    mUpgradeConfig.txMaxSizeBytes = 71680;
    mUpgradeConfig.bucketListSizeWindowSampleSize = 30;
    mUpgradeConfig.evictionScanSize = 100000;
    mUpgradeConfig.startingEvictionScanLevel = 7;

    setupAccountsAndUpgradeProtocol();

    setupUpgradeContract();

    upgradeSettings();

    setupLoadContracts();

    // One contract per account
    releaseAssert(mTxGenerator.getApplySorobanSuccess().count() ==
                  mNumAccounts + 4);
    releaseAssert(mTxGenerator.getApplySorobanFailure().count() == 0);
}

void
ApplyLoad::closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                       xdr::xvector<UpgradeType, 6> const& upgrades)
{
    auto txSet = makeTxSetFromTransactions(txs, mApp, 0, UINT64_MAX);

    auto sv =
        mApp.getHerder().makeStellarValue(txSet.first->getContentsHash(), 1,
                                          upgrades, mApp.getConfig().NODE_SEED);

    auto& lm = mApp.getLedgerManager();
    LedgerCloseData lcd(lm.getLastClosedLedgerNum() + 1, txSet.first, sv);
    lm.closeLedger(lcd);
}

void
ApplyLoad::setupAccountsAndUpgradeProtocol()
{
    auto const& lm = mApp.getLedgerManager();
    // pass in false for initialAccounts so we fund new account with a lower
    // balance, allowing the creation of more accounts.
    std::vector<Operation> creationOps = mTxGenerator.createAccounts(
        0, mNumAccounts, lm.getLastClosedLedgerNum() + 1, false);

    auto initTx = mTxGenerator.createTransactionFramePtr(mRoot, creationOps,
                                                         false, std::nullopt);

    // Upgrade to latest protocol as well
    auto upgrade = xdr::xvector<UpgradeType, 6>{};
    auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_VERSION};
    ledgerUpgrade.newLedgerVersion() = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    auto v = xdr::xdr_to_opaque(ledgerUpgrade);
    upgrade.push_back(UpgradeType{v.begin(), v.end()});

    closeLedger({initTx}, upgrade);
}

void
ApplyLoad::setupUpgradeContract()
{
    auto wasm = rust_bridge::get_write_bytes();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    mUpgradeCodeKey = contractCodeLedgerKey;

    auto const& lm = mApp.getLedgerManager();
    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt);

    closeLedger({uploadTx.second});

    auto salt = sha256("upgrade contract salt preimage");

    auto createTx = mTxGenerator.createContractTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
        wasmBytes.size() + 160, salt, std::nullopt);
    closeLedger({createTx.second});

    mUpgradeInstanceKey =
        createTx.second->sorobanResources().footprint.readWrite.back();
}

// To upgrade settings, just modify mUpgradeConfig and then call
// upgradeSettings()
void
ApplyLoad::upgradeSettings()
{
    auto const& lm = mApp.getLedgerManager();
    auto upgradeBytes =
        mTxGenerator.getConfigUpgradeSetFromLoadConfig(mUpgradeConfig);

    auto invokeTx = mTxGenerator.invokeSorobanCreateUpgradeTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, upgradeBytes, mUpgradeCodeKey,
        mUpgradeInstanceKey, std::nullopt);

    auto upgradeSetKey = mTxGenerator.getConfigUpgradeSetKey(
        mUpgradeConfig,
        mUpgradeInstanceKey.contractData().contract.contractId());

    auto upgrade = xdr::xvector<UpgradeType, 6>{};
    auto ledgerUpgrade = LedgerUpgrade{LEDGER_UPGRADE_CONFIG};
    ledgerUpgrade.newConfig() = upgradeSetKey;
    auto v = xdr::xdr_to_opaque(ledgerUpgrade);
    upgrade.push_back(UpgradeType{v.begin(), v.end()});

    closeLedger({invokeTx.second}, upgrade);
}

void
ApplyLoad::setupLoadContracts()
{
    auto wasm = rust_bridge::get_test_wasm_loadgen();
    xdr::opaque_vec<> wasmBytes;
    wasmBytes.assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(wasmBytes);

    mLoadCodeKey = contractCodeLedgerKey;

    auto const& lm = mApp.getLedgerManager();
    auto uploadTx = mTxGenerator.createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt);

    closeLedger({uploadTx.second});

    for (auto const& kvp : mTxGenerator.getAccounts())
    {
        auto salt = sha256("Load contract " + std::to_string(kvp.first));

        auto createTx = mTxGenerator.createContractTransaction(
            lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
            wasmBytes.size() + 160, salt, std::nullopt);
        closeLedger({createTx.second});

        auto instanceKey =
            createTx.second->sorobanResources().footprint.readWrite.back();

        TxGenerator::ContractInstance instance;
        instance.readOnlyKeys.emplace_back(mLoadCodeKey);
        instance.readOnlyKeys.emplace_back(instanceKey);
        instance.contractID = instanceKey.contractData().contract;
        mLoadInstances.emplace(kvp.first, instance);
    }
}

void
ApplyLoad::benchmark()
{
    auto& lm = mApp.getLedgerManager();
    std::vector<TransactionFrameBasePtr> txs;

    auto resources = multiplyByDouble(
        lm.maxLedgerResources(true), SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER);

    // Save a snapshot so we can calculate what % we used up.
    auto const resourcesSnapshot = resources;

    auto const& accounts = mTxGenerator.getAccounts();
    std::vector<uint64_t> shuffledAccounts(accounts.size());
    std::iota(shuffledAccounts.begin(), shuffledAccounts.end(), 0);
    stellar::shuffle(std::begin(shuffledAccounts), std::end(shuffledAccounts),
                     gRandomEngine);

    bool limitHit = false;
    for (auto accountIndex : shuffledAccounts)
    {
        auto it = accounts.find(accountIndex);
        releaseAssert(it != accounts.end());

        auto instanceIter = mLoadInstances.find(it->first);
        releaseAssert(instanceIter != mLoadInstances.end());
        auto const& instance = instanceIter->second;
        auto tx = mTxGenerator.invokeSorobanLoadTransaction(
            lm.getLastClosedLedgerNum() + 1, it->first, instance,
            rust_bridge::get_write_bytes().data.size() + 160, std::nullopt);

        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            auto res = tx.second->checkValid(mApp, ltx, 0, 0, UINT64_MAX);
            releaseAssert((res && res->isSuccess()));
        }

        if (!anyGreater(tx.second->getResources(false), resources))
        {
            resources -= tx.second->getResources(false);
        }
        else
        {
            for (size_t i = 0; i < resources.size(); ++i)
            {
                auto type = static_cast<Resource::Type>(i);
                if (tx.second->getResources(false).getVal(type) >
                    resources.getVal(type))
                {
                    CLOG_INFO(Perf, "Ledger {} limit hit during tx generation",
                              Resource::getStringFromType(type));
                    limitHit = true;
                }
            }

            break;
        }

        txs.emplace_back(tx.second);
    }

    // If this assert fails, it most likely means that we ran out of
    // accounts, which should not happen.
    releaseAssert(limitHit);

    mTxCountUtilization.Update(
        (1.0 - (resources.getVal(Resource::Type::OPERATIONS) * 1.0 /
                resourcesSnapshot.getVal(Resource::Type::OPERATIONS))) *
        100000.0);
    mInstructionUtilization.Update(
        (1.0 - (resources.getVal(Resource::Type::INSTRUCTIONS) * 1.0 /
                resourcesSnapshot.getVal(Resource::Type::INSTRUCTIONS))) *
        100000.0);
    mTxSizeUtilization.Update(
        (1.0 - (resources.getVal(Resource::Type::TX_BYTE_SIZE) * 1.0 /
                resourcesSnapshot.getVal(Resource::Type::TX_BYTE_SIZE))) *
        100000.0);
    mReadByteUtilization.Update(
        (1.0 - (resources.getVal(Resource::Type::READ_BYTES) * 1.0 /
                resourcesSnapshot.getVal(Resource::Type::READ_BYTES))) *
        100000.0);
    mWriteByteUtilization.Update(
        (1.0 - (resources.getVal(Resource::Type::WRITE_BYTES) * 1.0 /
                resourcesSnapshot.getVal(Resource::Type::WRITE_BYTES))) *
        100000.0);
    mReadEntryUtilization.Update(
        (1.0 -
         (resources.getVal(Resource::Type::READ_LEDGER_ENTRIES) * 1.0 /
          resourcesSnapshot.getVal(Resource::Type::READ_LEDGER_ENTRIES))) *
        100000.0);
    mWriteEntryUtilization.Update(
        (1.0 -
         (resources.getVal(Resource::Type::WRITE_LEDGER_ENTRIES) * 1.0 /
          resourcesSnapshot.getVal(Resource::Type::WRITE_LEDGER_ENTRIES))) *
        100000.0);

    closeLedger(txs);
}

double
ApplyLoad::successRate()
{
    return mTxGenerator.getApplySorobanSuccess().count() * 1.0 /
           (mTxGenerator.getApplySorobanSuccess().count() +
            mTxGenerator.getApplySorobanFailure().count());
}

medida::Histogram const&
ApplyLoad::getTxCountUtilization()
{
    return mTxCountUtilization;
}
medida::Histogram const&
ApplyLoad::getInstructionUtilization()
{
    return mInstructionUtilization;
}
medida::Histogram const&
ApplyLoad::getTxSizeUtilization()
{
    return mTxSizeUtilization;
}
medida::Histogram const&
ApplyLoad::getReadByteUtilization()
{
    return mReadByteUtilization;
}
medida::Histogram const&
ApplyLoad::getWriteByteUtilization()
{
    return mWriteByteUtilization;
}
medida::Histogram const&
ApplyLoad::getReadEntryUtilization()
{
    return mReadEntryUtilization;
}
medida::Histogram const&
ApplyLoad::getWriteEntryUtilization()
{
    return mWriteEntryUtilization;
}

}