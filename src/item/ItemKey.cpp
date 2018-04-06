// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "item/ItemKey.h"

namespace stellar
{

ItemKey::ItemKey(ItemType type, Hash hash) : mType{type}, mHash{hash}
{
}

ItemType
ItemKey::getType() const
{
    return mType;
}

Hash
ItemKey::getHash() const
{
    return mHash;
}

bool
operator==(ItemKey const& x, ItemKey const& y)
{
    if (x.mType != y.mType)
    {
        return false;
    }
    return x.mHash == y.mHash;
}

bool
operator<(ItemKey const& x, ItemKey const& y)
{
    if (x.mType < y.mType)
    {
        return true;
    }
    if (x.mType > y.mType)
    {
        return false;
    }
    return x.mHash < y.mHash;
}
}
