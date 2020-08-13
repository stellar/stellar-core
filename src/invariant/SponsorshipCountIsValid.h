#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include <memory>

namespace stellar
{

class Application;
class Database;
struct LedgerTxnDelta;

// This Invariant checks that the sponsorship state of each account is correct,
// and that the global sponsorship invariant
//      totalNumSponsoring = totalNumSponsored + totalClaimableBalanceReserve
// is respected.
class SponsorshipCountIsValid : public Invariant
{
  public:
    SponsorshipCountIsValid();

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta) override;
};
}
