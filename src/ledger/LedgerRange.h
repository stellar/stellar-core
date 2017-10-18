#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>

namespace stellar
{

class LedgerRange final
{
  public:
    LedgerRange(uint32_t first, uint32_t last);
    friend bool operator==(LedgerRange const& x, LedgerRange const& y);
    friend bool operator!=(LedgerRange const& x, LedgerRange const& y);

    uint32_t
    first() const
    {
        return mFirst;
    }
    uint32_t
    last() const
    {
        return mLast;
    }

  private:
    uint32_t mFirst;
    uint32_t mLast;
};
}
