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
    std::optional<uint32_t> maxContractSizeBytes{};
    std::optional<uint32_t> maxContractDataKeySizeBytes{};
    std::optional<uint32_t> maxContractDataEntrySizeBytes{};

    // Compute settings for contracts (instructions and memory).
    std::optional<int64_t> ledgerMaxInstructions{};
    std::optional<int64_t> txMaxInstructions{};
    std::optional<int64_t> feeRatePerInstructionsIncrement{};
    std::optional<uint32_t> txMemoryLimit{};
    std::optional<ContractCostParams> cpuCostParams{};
    std::optional<ContractCostParams> memCostParams{};

    // Ledger access settings for contracts.
    std::optional<uint32_t> ledgerMaxDiskReadEntries{};
    std::optional<uint32_t> ledgerMaxDiskReadBytes{};
    std::optional<uint32_t> ledgerMaxWriteLedgerEntries{};
    std::optional<uint32_t> ledgerMaxWriteBytes{};
    std::optional<int64_t> feeDiskReadLedgerEntry{};
    std::optional<int64_t> feeWriteLedgerEntry{};
    std::optional<int64_t> feeDiskRead1KB{};
    std::optional<uint32_t> ledgerMaxTxCount{};
    std::optional<uint32_t> txMaxDiskReadEntries{};
    std::optional<uint32_t> txMaxDiskReadBytes{};
    std::optional<uint32_t> txMaxWriteLedgerEntries{};
    std::optional<uint32_t> txMaxWriteBytes{};

    // Historical data (pushed to core archives) settings for contracts.
    std::optional<int64_t> feeHistorical1KB{};

    // Contract events settings.
    std::optional<uint32_t> txMaxContractEventsSizeBytes{};

    // Bandwidth related data settings for contracts
    std::optional<uint32_t> ledgerMaxTransactionsSizeBytes{};
    std::optional<uint32_t> txMaxSizeBytes{};
    std::optional<int64_t> feeTransactionSize1KB{};

    // State Archival Settings
    std::optional<uint32_t> maxEntryTTL{};
    std::optional<uint32_t> minTemporaryTTL{};
    std::optional<uint32_t> minPersistentTTL{};
    std::optional<int64_t> persistentRentRateDenominator{};
    std::optional<int64_t> tempRentRateDenominator{};
    std::optional<uint32_t> maxEntriesToArchive{};
    std::optional<uint32_t> liveSorobanStateSizeWindowSampleSize{};
    std::optional<uint32_t> liveSorobanStateSizeWindowSamplePeriod{};
    std::optional<uint32_t> evictionScanSize{};
    std::optional<uint32_t> startingEvictionScanLevel{};

    std::optional<int64_t> rentFee1KBSorobanStateSizeLow{};
    std::optional<int64_t> rentFee1KBSorobanStateSizeHigh{};

    // Parallel execution settings
    std::optional<uint32_t> ledgerMaxDependentTxClusters{};

    // Ledger cost extension settings
    std::optional<uint32_t> txMaxFootprintEntries{};
    std::optional<int64_t> feeFlatRateWrite1KB{};

    // SCP timing settings
    std::optional<uint32_t> ledgerTargetCloseTimeMilliseconds{};
    std::optional<uint32_t> nominationTimeoutInitialMilliseconds{};
    std::optional<uint32_t> nominationTimeoutIncrementMilliseconds{};
    std::optional<uint32_t> ballotTimeoutInitialMilliseconds{};
    std::optional<uint32_t> ballotTimeoutIncrementMilliseconds{};
};

class TxGenerator
{
  public:
    // Special account ID to represent the root account
    static uint64_t const ROOT_ACCOUNT_ID;

    struct ContractInstance
    {
        // [wasm, instance]
        xdr::xvector<LedgerKey> readOnlyKeys;
        SCAddress contractID;
        uint32_t contractEntriesSize = 0;
    };

    using TestAccountPtr = std::shared_ptr<TestAccount>;
    TxGenerator(Application& app, uint32_t prePopulatedArchivedEntries = 0);

    bool loadAccount(TestAccount& account);
    bool loadAccount(TestAccountPtr account);

    TestAccountPtr findAccount(uint64_t accountId, uint32_t ledgerNum);

    std::vector<Operation> createAccounts(uint64_t start, uint64_t count,
                                          uint32_t ledgerNum,
                                          bool initialAccounts);

    // Create transaction frame from `from` with `ops` and a generated fee
    // between the base fee required for `ops` and `maxGeneratedFeeRate` (if
    // present).
    TransactionFrameBaseConstPtr
    createTransactionFramePtr(TestAccountPtr from, std::vector<Operation> ops,
                              std::optional<uint32_t> maxGeneratedFeeRate);

    // Create transaction frame from `from` with `ops` and a generated fee
    // between the base fee required for `ops` and `maxGeneratedFeeRate` (if
    // present). Attempt to pad resulting size such that the serialization is
    // `byteCount`. Note that padding will not shrink a transaction, and that it
    // will always round up to the nearest multiple of four (since we are using
    // XDR).
    TransactionFrameBaseConstPtr
    createTransactionFramePtr(TestAccountPtr from, std::vector<Operation> ops,
                              std::optional<uint32_t> maxGeneratedFeeRate,
                              uint32_t byteCount);

    std::pair<TestAccountPtr, TransactionFrameBaseConstPtr>
    paymentTransaction(uint32_t numAccounts, uint32_t offset,
                       uint32_t ledgerNum, uint64_t sourceAccount,
                       uint32_t byteCount,
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
    createSACTransaction(uint32_t ledgerNum, std::optional<uint64_t> accountId,
                         Asset const& asset,
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
    invokeSACPayment(uint32_t ledgerNum, uint64_t fromAccountId,
                     SCAddress const& toAddress,
                     ContractInstance const& instance, uint64_t amount,
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

    TestAccountPtr getAccount(uint64_t accountId) const;
    void addAccount(uint64_t accountId, TestAccountPtr account);

  private:
    std::pair<SorobanResources, uint32_t> sorobanRandomUploadResources();

    void updateMinBalance();
    bool isLive(LedgerKey const& lk, uint32_t ledgerNum) const;

    Application& mApp;

    // Accounts cache
    std::map<uint64_t, TestAccountPtr> mAccounts;

    int64 mMinBalance;

    // Counts of soroban transactions that succeeded or failed at apply time
    medida::Counter const& mApplySorobanSuccess;
    medida::Counter const& mApplySorobanFailure;

    // mPrePopulatedArchivedEntries contains the
    // total number of pre-populated archived entries to autorestore for IO
    // load.
    uint32_t const mPrePopulatedArchivedEntries;

    // index of next entry to autorestore. LedgerKey can be derived from index
    // using ApplyLoad::getKeyForArchivedEntry.
    uint32_t mNextKeyToRestore{};
};

}
