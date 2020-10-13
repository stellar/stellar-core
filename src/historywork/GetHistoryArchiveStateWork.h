// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class HistoryArchive;
class GetRemoteFileWork;

class GetHistoryArchiveStateWork : public Work
{
    std::shared_ptr<GetRemoteFileWork> mGetRemoteFile;

    HistoryArchiveState mState;
    uint32_t mSeq;
    std::shared_ptr<HistoryArchive> mArchive;
    size_t mRetries;
    std::string mLocalFilename;

    medida::Meter& mGetHistoryArchiveStateSuccess;

    std::string getRemoteName() const;

  public:
    GetHistoryArchiveStateWork(
        Application& app, uint32_t seq = 0,
        std::shared_ptr<HistoryArchive> archive = nullptr,
        std::string mode = "", size_t maxRetries = BasicWork::RETRY_A_FEW);
    ~GetHistoryArchiveStateWork() = default;

    HistoryArchiveState const&
    getHistoryArchiveState() const
    {
        if (getState() != State::WORK_SUCCESS)
        {
            throw std::runtime_error("GetHistoryArchiveStateWork must succeed "
                                     "before state retrieval");
        }
        return mState;
    }

    std::shared_ptr<HistoryArchive>
    getArchive() const
    {
        return mArchive;
    }

    std::string getStatus() const override;

  protected:
    BasicWork::State doWork() override;
    void doReset() override;
    void onSuccess() override;
};
}
