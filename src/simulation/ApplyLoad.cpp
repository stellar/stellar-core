#include "simulation/ApplyLoad.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "test/TxTests.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "util/XDRCereal.h"
#include <crypto/SHA.h>

namespace stellar
{

ApplyLoad::ApplyLoad(Application& app, uint32_t numAccounts,
                     uint64_t ledgerMaxInstructions,
                     uint64_t ledgerMaxReadLedgerEntries,
                     uint64_t ledgerMaxReadBytes,
                     uint64_t ledgerMaxWriteLedgerEntries,
                     uint64_t ledgerMaxWriteBytes, uint64_t ledgerMaxTxCount,
                     uint64_t ledgerMaxTransactionsSizeBytes)
    : TxGenerator(app), mNumAccounts(numAccounts)
{
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

    createRootAccount();
    updateMinBalance();

    releaseAssert(mRoot);

    setupAccountsAndUpgradeProtocol();

    setupUpgradeContract();

    upgradeSettings();

    setupLoadContracts();

    // One contract per account
    releaseAssert(mApplySorobanSuccess.count() == numAccounts + 4);
    releaseAssert(mApplySorobanFailure.count() == 0);
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
    std::vector<Operation> creationOps =
        createAccounts(0, mNumAccounts, lm.getLastClosedLedgerNum() + 1, false);

    auto initTx =
        createTransactionFramePtr(mRoot, creationOps, false, std::nullopt);

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
    auto uploadTx = createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt);

    closeLedger({uploadTx.second});

    auto salt = sha256("upgrade contract salt preimage");

    auto createTx = createContractTransaction(
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
    auto upgradeBytes = getConfigUpgradeSetFromLoadConfig(mUpgradeConfig);

    auto invokeTx = invokeSorobanCreateUpgradeTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, upgradeBytes, mUpgradeCodeKey,
        mUpgradeInstanceKey, std::nullopt);

    auto upgradeSetKey = getConfigUpgradeSetKey(
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
    auto uploadTx = createUploadWasmTransaction(
        lm.getLastClosedLedgerNum() + 1, 0, wasmBytes, contractCodeLedgerKey,
        std::nullopt);

    closeLedger({uploadTx.second});

    for (auto kvp : mAccounts)
    {
        auto salt = sha256("Load contract " + std::to_string(kvp.first));

        auto createTx = createContractTransaction(
            lm.getLastClosedLedgerNum() + 1, 0, contractCodeLedgerKey,
            wasmBytes.size() + 160, salt, std::nullopt);
        closeLedger({createTx.second});

        auto instanceKey =
            createTx.second->sorobanResources().footprint.readWrite.back();

        ContractInstance instance;
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

    auto resources =
        multiplyByDouble(lm.maxLedgerResources(true),
                         2 /*TODO: use TRANSACTION_QUEUE_SIZE_MULTIPLIER*/);

    std::vector<uint64_t> shuffledAccounts(mAccounts.size());
    std::iota(shuffledAccounts.begin(), shuffledAccounts.end(), 0);
    stellar::shuffle(std::begin(shuffledAccounts), std::end(shuffledAccounts),
                     gRandomEngine);

    for (auto accountIndex : shuffledAccounts)
    {
        auto it = mAccounts.find(accountIndex);
        releaseAssert(it != mAccounts.end());

        auto instanceIter = mLoadInstances.find(it->first);
        releaseAssert(instanceIter != mLoadInstances.end());
        auto const& instance = instanceIter->second;
        auto tx = invokeSorobanLoadTransaction(
            lm.getLastClosedLedgerNum() + 1, it->first, instance,
            rust_bridge::get_write_bytes().data.size() + 160, std::nullopt);

        if (!anyGreater(tx.second->getResources(false), resources))
        {
            resources -= tx.second->getResources(false);
        }
        else
        {
            for (size_t i = static_cast<size_t>(Resource::Type::OPERATIONS);
                 i <= static_cast<size_t>(Resource::Type::WRITE_LEDGER_ENTRIES);
                 ++i)
            {
                auto type = static_cast<Resource::Type>(i);
                if (tx.second->getResources(false).getVal(type) >
                    resources.getVal(type))
                {
                    CLOG_INFO(Perf, "Ledger {} limit hit during tx generation",
                              Resource::getStringFromType(type));
                }
            }
            break;
        }

        txs.emplace_back(tx.second);
    }

    closeLedger(txs);
}

double
ApplyLoad::successRate()
{
    return mApplySorobanSuccess.count() * 1.0 /
           (mApplySorobanSuccess.count() + mApplySorobanFailure.count());
}

}