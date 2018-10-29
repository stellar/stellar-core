#include "main/Whitelist.h"
#include "transactions/SignatureUtils.h"
#include "ledger/DataFrame.h"
#include "transactions/TransactionFrame.h"
#include <stdint.h>
#include <unordered_map>

namespace stellar
{
Whitelist::Whitelist(Application& app)
{
    if (app.getConfig().WHITELIST.size() == 0)
        return;

    auto id = app.getConfig().WHITELIST;
    AccountID aid(KeyUtils::fromStrKey<PublicKey>(id));

    auto dfs = DataFrame::loadAllData(app.getDatabase(), aid);

    hash = std::unordered_map<uint32_t, std::vector<string64>>();

    for (auto& df : dfs)
    {
        auto data = df->getData();
        auto name = data.dataName;
        auto value = data.dataValue;

        int32_t intVal =
            (value[0] << 24) + (value[1] << 16) + (value[2] << 8) + value[3];

        std::vector<string64> keys = hash[intVal];
        keys.emplace_back(name);
        hash[intVal] = keys;
    }
}

bool
Whitelist::isWhitelisted(std::vector<DecoratedSignature> signatures, Hash const& txHash)
{
    for (auto& sig : signatures)
    {
        if (isWhitelistSig(sig, txHash))
            return true;
    }

    return false;
}

bool 
Whitelist::isWhitelistSig(DecoratedSignature const& sig, Hash const& txHash)
{
    int32_t hintInt = (sig.hint[0] << 24) + (sig.hint[1] << 16) +
                      (sig.hint[2] << 8) + sig.hint[3];

    auto it = hash.find(hintInt);
    if (it != hash.end())
    {
        for (auto key : it->second)
        {
            auto pkey = KeyUtils::fromStrKey<PublicKey>(key);

            if (PubKeyUtils::verifySig(pkey, sig.signature, txHash))
            {
                return true;
            }
        }
    }

	return false;
}
} // namespace stellar
