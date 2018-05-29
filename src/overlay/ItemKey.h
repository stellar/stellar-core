#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-types.h"

namespace stellar
{

enum class ItemType
{
    QUORUM_SET,
    TX_SET
};

class ItemKey
{
  public:
    ItemKey(ItemType type, Hash hash);

    ItemType getType() const;
    Hash getHash() const;

  private:
    friend bool operator==(ItemKey const& x, ItemKey const& y);
    friend bool operator<(ItemKey const& x, ItemKey const& y);

    ItemType mType;
    Hash mHash;
};
}
