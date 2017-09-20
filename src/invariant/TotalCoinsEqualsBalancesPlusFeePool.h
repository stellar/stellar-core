#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include <memory>

namespace stellar
{

class Application;
class Database;
class LedgerDelta;

class TotalCoinsEqualsBalancesPlusFeePool : public Invariant
{
  public:
    static const std::string kName;

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    explicit TotalCoinsEqualsBalancesPlusFeePool(Database& db);

    virtual std::string getName() const override;

    virtual std::string checkOnLedgerClose(LedgerDelta const& delta) override;

  private:
    Database& mDb;
};
}
