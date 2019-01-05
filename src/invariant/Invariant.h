#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <string>

namespace stellar
{

class Bucket;
struct LedgerTxnDelta;
struct Operation;
struct OperationResult;

// NOTE: The checkOn* functions should have a default implementation so that
//       more can be added in the future without requiring changes to all
//       derived classes.
class Invariant
{
    bool const mStrict;

  public:
    explicit Invariant(bool strict) : mStrict(strict)
    {
    }

    virtual ~Invariant()
    {
    }

    virtual std::string getName() const = 0;

    bool
    isStrict() const
    {
        return mStrict;
    }

    virtual std::string
    checkOnBucketApply(std::shared_ptr<Bucket const> bucket,
                       uint32_t oldestLedger, uint32_t newestLedger)
    {
        return std::string{};
    }

    virtual std::string
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& result,
                          LedgerTxnDelta const& ltxDelta)
    {
        return std::string{};
    }
};
}
