// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

class HistoryArchive;
struct HistoryArchiveState;

class GetHistoryArchiveStateWork : public Work
{
    HistoryArchiveState& mState;
    uint32_t mSeq;
    VirtualClock::duration mInitialDelay;
    std::shared_ptr<HistoryArchive const> mArchive;
    std::string mLocalFilename;

  public:
    GetHistoryArchiveStateWork(
        Application& app, WorkParent& parent, HistoryArchiveState& state,
        uint32_t seq = 0,
        VirtualClock::duration const& intitialDelay = std::chrono::seconds(0),
        std::shared_ptr<HistoryArchive const> archive = nullptr,
        size_t maxRetries = Work::RETRY_A_FEW);
    std::string getStatus() const override;
    VirtualClock::duration getRetryDelay() const override;
    void onReset() override;
    void onRun() override;
};
}
