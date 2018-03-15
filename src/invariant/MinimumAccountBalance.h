#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "ledger/LedgerState.h"
#include <memory>

namespace stellar
{
class Application;

// This Invariant is used to validate that accounts have the minimum balance.
// It is important to note that accounts can be below the minimum balance if
// the minimum balance increased since the last time the balance of those
// accounts decreased. Therefore, the Invariant only checks accounts that have
// had their balance decrease in the operation.
class MinimumAccountBalance : public Invariant
{
  public:
    MinimumAccountBalance();

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerState const& ls,
                          std::shared_ptr<LedgerHeaderReference const> header) override;

  private:
    bool shouldCheckBalance(LedgerState::IteratorValueType const& val) const;
};
}
