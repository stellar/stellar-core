#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include <memory>

namespace stellar
{

class Application;
struct LedgerTxnDelta;

// This Invariant has two purposes: to ensure that liabilities remain in sync
// with the offer book, and to ensure that the balance of accounts and
// trustlines respect the liabilities (and reserve). It is important to note
// that accounts can be below the minimum balance if the minimum balance
// increased since the last time the balance of those accounts decreased.
// Therefore, the Invariant only checks accounts that have had their balance
// decrease or their liabilities increase in the operation.
class LiabilitiesMatchOffers : public Invariant
{
  public:
    explicit LiabilitiesMatchOffers();

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta) override;
};
}
