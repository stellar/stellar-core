#pragma once

#include "xdr/Stellar-ledger-entries.h"

namespace stellar {

struct AssetPair
{
    Asset buying;
    Asset selling;
};
bool operator==(AssetPair const& lhs, AssetPair const& rhs);

struct AssetPairHash
{
    size_t operator()(AssetPair const& key) const;
};

} /* stellar */

