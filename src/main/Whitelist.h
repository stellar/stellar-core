#include "main/Application.h"
#include "main/ManagedDataCache.h"
#include "ledger/LedgerManager.h"

namespace stellar
{

class Whitelist : public ManagedDataCache
{
  public:
    static Whitelist* instance(Application& app);
    size_t unwhitelistedReserve(size_t setSize);

    bool isWhitelisted(std::vector<DecoratedSignature> signatures,
                       Hash const& txHash);
    bool isWhitelistSig(DecoratedSignature const& sig, Hash const& txHash);

    int getLastUpdated(){
        return lcl;
    }

    virtual std::string getAccount(Application& app) override;
    virtual void fulfill(std::vector<DataFrame::pointer> dfs) override;

  private:
    Whitelist()
    {
        lcl = 0;
    }
    static Whitelist* mINSTANCE;

    std::unordered_map<uint32_t, std::vector<string64>> whitelist;

	// default to a 5% reserve
	double mReserve = 0.05;
	int lcl;
};
} // namespace stellar
