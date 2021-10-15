#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{
class FileTransferInfo;
class GetAndUnzipRemoteFileWork;
class HistoryArchive;
class TmpDir;

// This class takes an LHHE for a given point in history (likely the LCL),
// downloads the containing checkpoint file from an archive and checks that the
// archive agrees with it. It exists for use by the offline self-check command.
class CheckSingleLedgerHeaderWork : public Work
{
  protected:
    void doReset() override;
    BasicWork::State doWork() override;
    void onFailureRaise() override;
    void onSuccess() override;

  public:
    CheckSingleLedgerHeaderWork(Application& app,
                                std::shared_ptr<HistoryArchive> archive,
                                LedgerHeaderHistoryEntry const& entry);
    ~CheckSingleLedgerHeaderWork();

  private:
    std::shared_ptr<HistoryArchive> const mArchive;
    LedgerHeaderHistoryEntry const mExpected;
    std::unique_ptr<TmpDir> mDownloadDir;
    std::unique_ptr<FileTransferInfo> mFt;
    std::shared_ptr<GetAndUnzipRemoteFileWork> mGetLedgerFileWork;
    medida::Meter& mCheckSuccess;
    medida::Meter& mCheckFailed;
};
}
