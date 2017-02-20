// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/BucketDownloadWork.h"

namespace stellar
{

class CatchupWork : public BucketDownloadWork
{
  protected:
    HistoryArchiveState mRemoteState;
    uint32_t const mInitLedger;
    bool const mManualCatchup;
    std::shared_ptr<Work> mGetHistoryArchiveStateWork;

    uint32_t nextLedger() const;
    virtual uint32_t archiveStateSeq() const;
    virtual uint32_t firstCheckpointSeq() const = 0;
    virtual uint32_t lastCheckpointSeq() const;

  public:
    CatchupWork(Application& app, WorkParent& parent, uint32_t initLedger,
                std::string const& mode, bool manualCatchup);
    virtual void onReset() override;
};
}
