#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/BasicWork.h"

namespace stellar
{

/*
 * WorkSequence is a helper class, that implementers can use if they
 * wish to enforce the order of work execution. Users are required to pass
 * a vector of BasicWork pointers in the expected execution order.
 */
class WorkSequence : public BasicWork
{
    std::vector<std::shared_ptr<BasicWork>> mSequenceOfWork;
    std::vector<std::shared_ptr<BasicWork>>::const_iterator mNextInSequence;
    std::shared_ptr<BasicWork> mCurrentExecuting;

  public:
    WorkSequence(Application& app, std::string name,
                 std::vector<std::shared_ptr<BasicWork>> sequence,
                 size_t maxRetries = RETRY_A_FEW);
    ~WorkSequence() = default;

    std::string getStatus() const override;
    void shutdown() override;

  protected:
    State onRun() final;
    bool onAbort() final;
    void onReset() final;
};
}
