// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

#include "history/FileTransferInfo.h"

namespace stellar
{

class HistoryArchive;

class GetAndUnzipRemoteFileWork : public Work
{
    std::shared_ptr<Work> mGetRemoteFileWork;
    std::shared_ptr<Work> mGunzipFileWork;

    FileTransferInfo mFt;
    std::shared_ptr<HistoryArchive> mArchive;

  public:
    // Passing `nullptr` for the archive argument will cause the work to
    // select a new readable history archive at random each time it runs /
    // retries.
    GetAndUnzipRemoteFileWork(Application& app, WorkParent& parent,
                              FileTransferInfo ft,
                              std::shared_ptr<HistoryArchive> archive = nullptr,
                              size_t maxRetries = Work::RETRY_A_LOT);
    ~GetAndUnzipRemoteFileWork();
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};
}
