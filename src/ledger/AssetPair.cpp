#include "ledger/AssetPair.h"

#include "util/XDROperators.h"

#include "ledger/LedgerHashUtils.h"


namespace stellar {

bool
operator==(AssetPair const& lhs, AssetPair const& rhs)
{
    return lhs.buying == rhs.buying && lhs.selling == rhs.selling;
}

size_t
AssetPairHash::operator()(AssetPair const& key) const
{
    std::hash<Asset> hashAsset;
    return hashAsset(key.buying) ^ (hashAsset(key.selling) << 1);
}

} /* stellar */

