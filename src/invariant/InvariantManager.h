#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "lib/json/json.h"
#include <memory>

namespace stellar
{

class Application;
class Invariant;
class LedgerDelta;
struct Operation;

/**
 * InvariantManager maintains a registry of available invariants and
 * supports enabling them dynamically, such as at configuration time.
 * When the appropriate event, such as a ledger close, triggers the
 * InvariantManager it will check each of the enabled invariants and
 * throw InvariantDoesNotHold if any are violated.
 */
class InvariantManager
{
  public:
    static std::unique_ptr<InvariantManager> create(Application& app);

    virtual ~InvariantManager()
    {
    }

    virtual Json::Value getInformation() = 0;

    virtual void checkOnBucketApply(std::shared_ptr<Bucket const> bucket,
                                    uint32_t ledger, uint32_t level,
                                    bool isCurr) = 0;

    virtual void checkOnOperationApply(Operation const& operation,
                                       OperationResult const& opres,
                                       LedgerDelta const& delta) = 0;

    virtual void registerInvariant(std::shared_ptr<Invariant> invariant) = 0;

    virtual void enableInvariant(std::string const& name) = 0;

    template <typename T, typename... Args>
    std::shared_ptr<T>
    registerInvariant(Args&&... args)
    {
        auto invariant = std::make_shared<T>(std::forward<Args>(args)...);
        registerInvariant(invariant);
        return invariant;
    }
};
}
