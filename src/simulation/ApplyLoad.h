#include "main/Application.h"
#include "simulation/TxGenerator.h"
#include "test/TestAccount.h"

namespace stellar
{
class ApplyLoad
{
  public:
    ApplyLoad(Application& app, uint32_t numAccounts,
              uint64_t ledgerMaxInstructions,
              uint64_t ledgerMaxReadLedgerEntries, uint64_t ledgerMaxReadBytes,
              uint64_t ledgerMaxWriteLedgerEntries,
              uint64_t ledgerMaxWriteBytes, uint64_t ledgerMaxTxCount,
              uint64_t ledgerMaxTransactionsSizeBytes);

    void benchmark();

    double successRate();

  private:
    void closeLedger(std::vector<TransactionFrameBasePtr> const& txs,
                     xdr::xvector<UpgradeType, 6> const& upgrades = {});

    void setupAccountsAndUpgradeProtocol();
    void setupUpgradeContract();
    void setupLoadContracts();

    // Upgrades using mUpgradeConfig
    void upgradeSettings();

    LedgerKey mUpgradeCodeKey;
    LedgerKey mUpgradeInstanceKey;

    LedgerKey mLoadCodeKey;
    UnorderedMap<uint64_t, TxGenerator::ContractInstance> mLoadInstances;

    SorobanUpgradeConfig mUpgradeConfig;

    TxGenerator mTxGenerator;
    Application& mApp;
    TxGenerator::TestAccountPtr mRoot;

    uint32_t mNumAccounts;
};

}