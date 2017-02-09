#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchive.h"
#include "util/Timer.h"
#include "util/TmpDir.h"

#include <map>
#include <memory>
#include <vector>

namespace stellar
{

class FileTransferInfo;

struct StateSnapshot : public std::enable_shared_from_this<StateSnapshot>
{
    Application& mApp;
    HistoryArchiveState mLocalState;
    TmpDir mSnapDir;
    std::shared_ptr<FileTransferInfo> mLedgerSnapFile;
    std::shared_ptr<FileTransferInfo> mTransactionSnapFile;
    std::shared_ptr<FileTransferInfo> mTransactionResultSnapFile;
    std::shared_ptr<FileTransferInfo> mSCPHistorySnapFile;

    StateSnapshot(Application& app, HistoryArchiveState const& state);
    void makeLive();
    bool writeHistoryBlocks() const;
};
}
