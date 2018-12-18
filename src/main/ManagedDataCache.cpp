#include "main/ManagedDataCache.h"
#include "ledger/DataFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrame.h"
#include <stdint.h>
#include <unordered_map>

namespace stellar
{
std::shared_ptr<AccountID>
ManagedDataCache::accountID()
{
    auto account = getAccount();
    if (account.size() == 0)
        return std::shared_ptr<AccountID>(nullptr);

    return std::make_shared<AccountID>(KeyUtils::fromStrKey<PublicKey>(account));
}

void
ManagedDataCache::update()
{
    auto id = getAccount();

    if (id.size() == 0 || needsUpdate == false)
        return;

    AccountID aid(KeyUtils::fromStrKey<PublicKey>(id));
    auto dfs = DataFrame::loadAccountData(mApp.getDatabase(), aid);

    // Handle dataframe objects
    fulfill(dfs);
    needsUpdate = false;
}
}
