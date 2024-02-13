#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/BatchDownloadWork.h"
#include "work/BasicWork.h"
#include <memory>

namespace stellar
{
// A version of BatchDownloadWork that does not fail if a download fails.
class BestEffortBatchDownloadWork : public BatchDownloadWork
{
  public:
    BestEffortBatchDownloadWork(
        Application& app, CheckpointRange range, std::string const& type,
        std::shared_ptr<TmpDir const> downloadDir,
        std::shared_ptr<HistoryArchive> archive = nullptr);
    ~BestEffortBatchDownloadWork() = default;

  protected:
    State onChildFailure() override;
};

} // namespace stellar