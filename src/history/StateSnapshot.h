#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "util/TmpDir.h"
#include "history/HistoryArchive.h"

#include <map>
#include <memory>
#include <vector>

namespace stellar
{

enum FilePublishState
{
    FILE_PUBLISH_FAILED = -1,
    FILE_PUBLISH_NEEDED = 0,
    FILE_PUBLISH_MAYBE_NEEDED = 1, // optional publish
    FILE_PUBLISH_COMPRESSING = 3,
    FILE_PUBLISH_COMPRESSED = 4,
    FILE_PUBLISH_MAKING_DIR = 5,
    FILE_PUBLISH_MADE_DIR = 6,
    FILE_PUBLISH_UPLOADING = 7,
    FILE_PUBLISH_UPLOADED = 8,
    FILE_PUBLISH_COMPLETE = 9
};

template <typename T> class FileTransferInfo;

typedef FileTransferInfo<FilePublishState> FilePublishInfo;

struct StateSnapshot : public std::enable_shared_from_this<StateSnapshot>
{
    Application& mApp;
    HistoryArchiveState mLocalState;
    TmpDir mSnapDir;
    std::shared_ptr<FilePublishInfo> mLedgerSnapFile;
    std::shared_ptr<FilePublishInfo> mTransactionSnapFile;
    std::shared_ptr<FilePublishInfo> mTransactionResultSnapFile;
    std::shared_ptr<FilePublishInfo> mSCPHistorySnapFile;

    VirtualTimer mRetryTimer;
    size_t mRetryCount{0};

    StateSnapshot(Application& app, HistoryArchiveState const& state);
    void makeLive();
    bool writeHistoryBlocks() const;
    void writeHistoryBlocksWithRetry();
    void retryHistoryBlockWriteOrFail(asio::error_code const& ec);
};

}
