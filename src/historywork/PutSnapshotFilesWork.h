// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "util/UnorderedSet.h"
#include "work/Work.h"

namespace stellar
{

struct StateSnapshot;
class GetHistoryArchiveStateWork;

class PutSnapshotFilesWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;

    // Keep track of each step
    std::list<std::shared_ptr<GetHistoryArchiveStateWork>> mGetStateWorks;
    std::list<std::shared_ptr<BasicWork>> mGzipFilesWorks;
    std::list<std::shared_ptr<BasicWork>> mUploadSeqs;

    UnorderedSet<std::string> getFilesToZip();

  public:
    PutSnapshotFilesWork(Application& app,
                         std::shared_ptr<StateSnapshot> snapshot);
    ~PutSnapshotFilesWork() = default;

    std::string getStatus() const override;

  protected:
    State doWork() override;
    void doReset() override;
};
}
