#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstddef>
#include <list>
#include <queue>

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
    SyncingLedgerChain();
    SyncingLedgerChain(SyncingLedgerChain const&);
    ~SyncingLedgerChain();

    LedgerCloseData const& front() const;
    LedgerCloseData const& back() const;
    void pop();
    SyncingLedgerChainAddResult push(LedgerCloseData lcd);

    size_t size() const;
    bool empty() const;

  private:
    std::queue<LedgerCloseData, std::list<LedgerCloseData>> mChain;
};
}
