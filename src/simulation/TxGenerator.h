#pragma once

#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"

namespace medida
{
class Counter;
}
namespace stellar
{
// Calculates total size we'll need to read for all specified keys
uint64_t footprintSize(Application& app,
                       xdr::xvector<stellar::LedgerKey> const& keys);

// Config settings for SOROBAN_CREATE_UPGRADE
struct SorobanUpgradeConfig
{
    // Network Upgrade Parameters
    uint32_t maxContractSizeBytes{};
    uint32_t maxContractDataKeySizeBytes{};
    uint32_t maxContractDataEntrySizeBytes{};

    // Compute settings for contracts (instructions and memory).
    int64_t ledgerMaxInstructions{};
    int64_t txMaxInstructions{};
    uint32_t txMemoryLimit{};

    // Ledger access settings for contracts.
    uint32_t ledgerMaxReadLedgerEntries{};
    uint32_t ledgerMaxReadBytes{};
    uint32_t ledgerMaxWriteLedgerEntries{};
    uint32_t ledgerMaxWriteBytes{};
    uint32_t ledgerMaxTxCount{};
    uint32_t txMaxReadLedgerEntries{};
    uint32_t txMaxReadBytes{};
    uint32_t txMaxWriteLedgerEntries{};
    uint32_t txMaxWriteBytes{};

    // Contract events settings.
    uint32_t txMaxContractEventsSizeBytes{};

    // Bandwidth related data settings for contracts
    uint32_t ledgerMaxTransactionsSizeBytes{};
    uint32_t txMaxSizeBytes{};

    // State Archival Settings
    uint32_t maxEntryTTL{};
    uint32_t minTemporaryTTL{};
    uint32_t minPersistentTTL{};
    int64_t persistentRentRateDenominator{};
    int64_t tempRentRateDenominator{};
    uint32_t maxEntriesToArchive{};
    uint32_t bucketListSizeWindowSampleSize{};
    uint32_t bucketListWindowSamplePeriod{};
    uint32_t evictionScanSize{};
    uint32_t startingEvictionScanLevel{};
};

class TxGenerator
{
  public:
    struct ContractInstance
    {
        // [wasm, instance]
        xdr::xvector<LedgerKey> readOnlyKeys;
        SCAddress contractID;
        uint32_t contractEntriesSize = 0;
    };

    using TestAccountPtr = std::shared_ptr<TestAccount>;
    TxGenerator(Application& app);

    bool loadAccount(TestAccount& account);
    bool loadAccount(TestAccountPtr account);

    TestAccountPtr findAccount(uint64_t accountId, uint32_t ledgerNum);

    std::vector<Operation> createAccounts(uint64_t start, uint64_t count,
                                          uint32_t ledgerNum,
                                          bool initialAccounts);

    TransactionFrameBaseConstPtr
    createTransactionFramePtr(TestAccountPtr from, std::vector<Operation> ops,
                              bool pretend,
                              std::optional<uint32_t> maxGeneratedFeeRate);

    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    paymentTransaction(uint32_t numAccounts, uint32_t offset,
                       uint32_t ledgerNum, uint64_t sourceAccount,
                       uint32_t opCount,
                       std::optional<uint32_t> maxGeneratedFeeRate);

    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    manageOfferTransaction(uint32_t ledgerNum, uint64_t accountId,
                           uint32_t opCount,
                           std::optional<uint32_t> maxGeneratedFeeRate);

    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    createUploadWasmTransaction(
        uint32_t ledgerNum, uint64_t accountId, xdr::opaque_vec<> const& wasm,
        LedgerKey const& contractCodeLedgerKey,
        std::optional<uint32_t> maxGeneratedFeeRate,
        std::optional<SorobanResources> resources = std::nullopt);
    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    createContractTransaction(uint32_t ledgerNum, uint64_t accountId,
                              LedgerKey const& codeKey,
                              uint64_t contractOverheadBytes,
                              uint256 const& salt,
                              std::optional<uint32_t> maxGeneratedFeeRate);

    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    invokeSorobanLoadTransaction(uint32_t ledgerNum, uint64_t accountId,
                                 TxGenerator::ContractInstance const& instance,
                                 uint64_t contractOverheadBytes,
                                 std::optional<uint32_t> maxGeneratedFeeRate);
    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    invokeSorobanLoadTransactionV2(uint32_t ledgerNum, uint64_t accountId,
                                   ContractInstance const& instance,
                                   uint64_t dataEntryCount,
                                   size_t dataEntrySize,
                                   std::optional<uint32_t> maxGeneratedFeeRate);
    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    invokeSorobanCreateUpgradeTransaction(
        uint32_t ledgerNum, uint64_t accountId, SCBytes const& upgradeBytes,
        LedgerKey const& codeKey, LedgerKey const& instanceKey,
        std::optional<uint32_t> maxGeneratedFeeRate,
        std::optional<SorobanResources> resources = std::nullopt);
    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    sorobanRandomWasmTransaction(uint32_t ledgerNum, uint64_t accountId,
                                 uint32_t inclusionFee);

    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    pretendTransaction(uint32_t numAccounts, uint32_t offset,
                       uint32_t ledgerNum, uint64_t sourceAccount,
                       uint32_t opCount,
                       std::optional<uint32_t> maxGeneratedFeeRate);

    int generateFee(std::optional<uint32_t> maxGeneratedFeeRate, size_t opsCnt);

    std::pair<TestAccountPtr, TestAccountPtr>
    pickAccountPair(uint32_t numAccounts, uint32_t offset, uint32_t ledgerNum,
                    uint64_t sourceAccountId);

    ConfigUpgradeSetKey
    getConfigUpgradeSetKey(SorobanUpgradeConfig const& upgradeCfg,
                           Hash const& contractId) const;

    SCBytes getConfigUpgradeSetFromLoadConfig(
        SorobanUpgradeConfig const& upgradeCfg) const;

    std::map<uint64_t, TestAccountPtr> const& getAccounts();

    medida::Counter const& getApplySorobanSuccess();
    medida::Counter const& getApplySorobanFailure();

    void reset();

  private:
    std::pair<SorobanResources, uint32_t> sorobanRandomUploadResources();

    void updateMinBalance();

    Application& mApp;

    // Accounts cache
    std::map<uint64_t, TestAccountPtr> mAccounts;

    int64 mMinBalance;

    // Counts of soroban transactions that succeeded or failed at apply time
    medida::Counter const& mApplySorobanSuccess;
    medida::Counter const& mApplySorobanFailure;
};

}
