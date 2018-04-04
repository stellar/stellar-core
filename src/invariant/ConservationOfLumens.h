#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "xdr/Stellar-ledger-entries.h"
#include <memory>

namespace stellar
{

class Application;
class LedgerManager;

// This Invariant is used to validate that the total number of lumens only
// changes during inflation. The Invariant also checks that, after inflation,
// the totalCoins and feePool of the LedgerHeader matches the total balance
// in the database.
class ConservationOfLumens : public Invariant
{
  public:
    ConservationOfLumens();

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerState const& ls,
                          std::shared_ptr<LedgerHeaderReference const> header) override;

  private:
    int64_t calculateDeltaBalance(std::shared_ptr<LedgerEntry const> const& current,
                                  std::shared_ptr<LedgerEntry const> const& previous) const;
};
}
