#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantManager.h"
#include "util/make_unique.h"
#include <map>
#include <vector>

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{
class LedgerState;

class InvariantManagerImpl : public InvariantManager
{
    std::map<std::string, std::shared_ptr<Invariant>> mInvariants;
    std::vector<std::shared_ptr<Invariant>> mEnabled;
    medida::MetricsRegistry& mMetricsRegistry;

    struct InvariantFailureInformation
    {
        uint32_t lastFailedOnLedger;
        std::string lastFailedWithMessage;
    };
    std::map<std::string, InvariantFailureInformation> mFailureInformation;

  public:
    InvariantManagerImpl(medida::MetricsRegistry& registry);

    virtual Json::Value getInformation() override;

    void checkOnOperationApply(
        Operation const& operation, OperationResult const& opres,
        LedgerState const& ls,
        std::shared_ptr<LedgerHeaderReference const> header) override;

    void checkOnBucketApply(std::shared_ptr<Bucket const> bucket,
                            uint32_t ledger, uint32_t level,
                            bool isCurr) override;

    virtual void
    registerInvariant(std::shared_ptr<Invariant> invariant) override;

    virtual void enableInvariant(std::string const& name) override;

  private:
    void onInvariantFailure(std::shared_ptr<Invariant> invariant,
                            std::string const& message, uint32_t ledger);

    virtual void handleInvariantFailure(std::shared_ptr<Invariant> invariant,
                                        std::string const& message) const;
};
}
