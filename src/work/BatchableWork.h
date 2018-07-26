// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "lib/util/format.h"
#include "work/Work.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

class BatchableWork : public Work
{
    // Class that provides skeleton for snapshot classes.
    // Child classes can create dependencies by utilizing
    // registerDependent and notifyCompleted and overriding unblockWork. This is
    // useful if we need to enforce a DAG in the work structure. For example,
    // when verifying ledger chain, current work needs to wait for the previous
    // work to complete in order to proceed.

  protected:
    struct BatchableWorkResultData
    {
        LedgerHeaderHistoryEntry mVerifiedAheadLedger;
        // ...add more useful stuff...
    };

    std::shared_ptr<BatchableWork> mDependentSnapshot;
    BatchableWorkResultData mSnapshotData{};

  public:
    BatchableWork(Application& app, WorkParent& parent, std::string name,
                  size_t maxRetries = RETRY_A_FEW);
    ~BatchableWork() override;
    void registerDependent(std::shared_ptr<BatchableWork> blockedWork);
    void notifyCompleted();
    std::shared_ptr<BatchableWork> getDependent();

    // Each subclass decides how to proceed
    virtual void unblockWork(BatchableWorkResultData const& data);
};
}
