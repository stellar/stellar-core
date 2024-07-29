#include "main/Application.h"
#include "simulation/TxGenerator.h"
#include "test/TestAccount.h"

namespace stellar
{
class ApplyLoad : public TxGenerator
{
  public:
    ApplyLoad(Application& app);
    void benchmark();

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
    UnorderedMap<uint64_t, ContractInstance> mLoadInstances;

    SorobanUpgradeConfig mUpgradeConfig;
};

}