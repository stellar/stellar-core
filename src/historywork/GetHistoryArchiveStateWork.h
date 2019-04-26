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
    std::shared_ptr<BasicWork> mGetRemoteFile;

    HistoryArchiveState& mState;
    uint32_t mSeq;
    std::shared_ptr<HistoryArchive> mArchive;
    size_t mRetries;
    std::string mLocalFilename;

    medida::Meter& mGetHistoryArchiveStateSuccess;

  public:
    GetHistoryArchiveStateWork(
        Application& app, HistoryArchiveState& state, uint32_t seq = 0,
        std::shared_ptr<HistoryArchive> archive = nullptr,
        size_t maxRetries = BasicWork::RETRY_A_FEW);
    ~GetHistoryArchiveStateWork() = default;

  protected:
    BasicWork::State doWork() override;
    void doReset() override;
    void onSuccess() override;
};
}
