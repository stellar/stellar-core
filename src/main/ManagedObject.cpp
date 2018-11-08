#include "main/Whitelist.h"
#include "ledger/DataFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrame.h"
#include <stdint.h>
#include <unordered_map>

namespace stellar
{
void
ManagedObject::update(Application& app)
{
    if (getAccount(app).size() == 0 || lcl >= app.getLedgerManager().getLedgerNum()){
        return;
    }

    auto id = getAccount(app);
    AccountID aid(KeyUtils::fromStrKey<PublicKey>(id));
    auto dfs = DataFrame::loadAllData(app.getDatabase(), aid);

    // Handle dataframe objects
    fulfill(dfs);
    lcl = app.getLedgerManager().getLastClosedLedgerNum();
}

}
