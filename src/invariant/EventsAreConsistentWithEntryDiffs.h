#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/Invariant.h"
#include "main/Config.h"
#include "transactions/TransactionUtils.h"
#include <memory>

namespace stellar
{

class Application;
struct LedgerTxnDelta;

class EventsAreConsistentWithEntryDiffs : public Invariant
{
    Hash const& mNetworkID;

  public:
    EventsAreConsistentWithEntryDiffs(Hash const& networkID);

    static std::shared_ptr<Invariant> registerInvariant(Application& app);

    virtual std::string getName() const override;

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta,
                          std::vector<ContractEvent> const& events) override;
};
}