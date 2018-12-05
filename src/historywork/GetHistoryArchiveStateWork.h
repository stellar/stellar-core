// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class HistoryArchive;
struct HistoryArchiveState;

class GetHistoryArchiveStateWork : public Work
{
    HistoryArchiveState& mState;
    uint32_t mSeq;
    std::shared_ptr<HistoryArchive> mArchive;
    std::string mLocalFilename;

    medida::Meter& mGetHistoryArchiveStateSuccess;

  public:
    GetHistoryArchiveStateWork(
        Application& app, WorkParent& parent, std::string uniqueName,
        HistoryArchiveState& state, uint32_t seq = 0,
        std::shared_ptr<HistoryArchive> archive = nullptr,
        size_t maxRetries = Work::RETRY_A_FEW);
    ~GetHistoryArchiveStateWork();
    std::string getStatus() const override;
    void onReset() override;
    void onRun() override;

    State onSuccess() override;
    void onFailureRetry() override;
    void onFailureRaise() override;
};
}
