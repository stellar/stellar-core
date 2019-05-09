#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/optional.h"
#include "xdr/Stellar-types.h"
#include <cstdint>

namespace stellar
{
// Ledger seq num + hash pair, a lightweight substitute of ledger
// history entry, useful for catchup and ledger verification purposes.
using LedgerNumHashPair = std::pair<uint32_t, optional<Hash>>;

struct LedgerRange final
{
  public:
    uint32_t const mFirst;
    uint32_t const mLast;

    LedgerRange(uint32_t first, uint32_t last);
    std::string toString() const;

    friend bool operator==(LedgerRange const& x, LedgerRange const& y);
    friend bool operator!=(LedgerRange const& x, LedgerRange const& y);
};
}
