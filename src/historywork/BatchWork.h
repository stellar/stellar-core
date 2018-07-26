// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "work/BatchableWork.h"
#include "work/Work.h"

namespace stellar
{

class TmpDir;

class BatchWork : public Work
{
    /* This class performs parallel batching of Work by throttling workers.
       Child classes must supply iteration methods, that would generate work
       they'd like to perform. This class only acts as a commander, adding more
       work if it has bandwidth.
    **/
    void handleNewWork(bool isFirst);

    // Keep track of last assigned work as any next work will depend on it
    // (has to be strong reference in case it finished prior to new work
    // assigned)
    std::shared_ptr<BatchableWork> mLastAssignedWork;

  public:
    BatchWork(Application& app, WorkParent& parent, std::string name);
    ~BatchWork() override;
    void onReset() override;

    // Note: If a subclass chooses to override notify, it needs to make sure
    // it calls notify of BatchWork.
    void notify(std::string const& child) override;

  protected:
    virtual bool hasNext() = 0;
    virtual std::shared_ptr<BatchableWork> yieldMoreWork() = 0;
    virtual void resetIter() = 0;
};
}
