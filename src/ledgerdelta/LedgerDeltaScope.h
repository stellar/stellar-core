#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

class LedgerDelta;

class LedgerDeltaScope
{
  public:
    explicit LedgerDeltaScope(LedgerDelta& stack);
    ~LedgerDeltaScope();

    LedgerDeltaScope(LedgerDeltaScope const&) = delete;
    LedgerDeltaScope(LedgerDeltaScope&&) = delete;

    LedgerDeltaScope& operator=(LedgerDeltaScope const&) = delete;
    LedgerDeltaScope& operator=(LedgerDeltaScope&&) = delete;

    void commit();

  private:
    LedgerDelta& mStack;
    bool mCommited{false};
};
}
