#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"

namespace stellar
{

class Database;
class LedgerDeltaLayer;

class TotalCoinsEqualsBalancesPlusFeePool : public Invariant
{
  public:
    explicit TotalCoinsEqualsBalancesPlusFeePool(Database& db);
    virtual ~TotalCoinsEqualsBalancesPlusFeePool() override;

    virtual std::string getName() const override;
    virtual std::string check(LedgerDelta const& delta) const override;

  private:
    Database& mDb;
};
}
