// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/FileTransferInfo.h"
#include "work/Work.h"

namespace stellar
{

class HistoryArchive;

class GetAndUnzipRemoteFileWork : public Work
{
    std::shared_ptr<Work> mGetRemoteFileWork;
    std::shared_ptr<Work> mGunzipFileWork;

    FileTransferInfo mFt;
    std::shared_ptr<HistoryArchive> mArchive;

    bool validateFile();

  public:
    // Passing `nullptr` for the archive argument will cause the work to
    // select a new readable history archive at random each time it runs /
    // retries.
    GetAndUnzipRemoteFileWork(Application& app, std::function<void()> callback,
                              FileTransferInfo ft,
                              std::shared_ptr<HistoryArchive> archive = nullptr,
                              size_t maxRetries = BasicWork::RETRY_A_LOT);
    ~GetAndUnzipRemoteFileWork() = default;
    std::string getStatus() const override;

  protected:
    void doReset() override;
    void onFailureRaise() override;
    void onFailureRetry() override;
    State doWork() override;
};
}
