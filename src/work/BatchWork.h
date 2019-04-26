// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "work/Work.h"

namespace stellar
{
/* This class performs parallel batching of Work by throttling workers.
   Child classes must supply iteration methods, that would generate work
   they'd like to perform. This class is simply a coordinator.

   Batch work will:
   * Terminate with a failure if _any_ child work failed
   * Finish only if all children finished
   * Add more more if number of running children is less than bandwidth
**/
class BatchWork : public Work
{
    // Keep track of children here
    std::map<std::string, std::shared_ptr<BasicWork>> mBatch;
    void addMoreWorkIfNeeded();

  public:
    BatchWork(Application& app, std::string name);
    ~BatchWork() = default;

    size_t
    getNumWorksInBatch() const
    {
        return mBatch.size();
    }

  protected:
    void doReset() final;
    State doWork() final;

    // Implementers aren't allowed to edit batching mechanism,
    // but instead provide iteration methods.
    virtual bool hasNext() const = 0;
    virtual std::shared_ptr<BasicWork> yieldMoreWork() = 0;
    virtual void resetIter() = 0;
};
}
