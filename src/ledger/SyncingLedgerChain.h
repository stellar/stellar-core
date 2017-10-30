#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>

namespace stellar
{

class LedgerCloseData;

enum class SyncingLedgerChainAddResult
{
    CONTIGUOUS,
    TOO_OLD,
    TOO_NEW
};

class SyncingLedgerChain final
{
  public:
    using storage = std::vector<LedgerCloseData>;
    using const_iterator = storage::const_iterator;
    using size_type = storage::size_type;

    SyncingLedgerChain();
    SyncingLedgerChain(SyncingLedgerChain const&);
    ~SyncingLedgerChain();

    SyncingLedgerChainAddResult add(LedgerCloseData lcd);

    LedgerCloseData const& front() const;
    LedgerCloseData const& back() const;
    bool empty() const;
    size_type size() const;
    const_iterator begin() const;
    const_iterator end() const;

  private:
    storage mChain;
};
}
