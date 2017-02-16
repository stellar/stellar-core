// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

class TmpDir;

class BatchDownloadWork : public Work
{
    // Specialized class for downloading _lots_ of files (thousands to
    // millions). Sets up N (small number) of parallel download-decompress
    // worker chains to nibble away at a set of files-to-download, stored
    // as an integer deque. N is the subprocess-concurrency limit by default
    // (though it's still enforced globally at the ProcessManager level,
    // so you don't have to worry about making a few extra BatchDownloadWork
    // classes -- they won't override the global limit, just schedule a small
    // backlog in the ProcessManager).
    std::deque<uint32_t> mFinished;
    std::map<std::string, uint32_t> mRunning;
    uint32_t mFirst;
    uint32_t mLast;
    uint32_t mNext;
    std::string mFileType;
    TmpDir const& mDownloadDir;

    void addNextDownloadWorker();

  public:
    BatchDownloadWork(Application& app, WorkParent& parent, uint32_t first,
                      uint32_t last, std::string const& type,
                      TmpDir const& downloadDir);
    std::string getStatus() const override;
    void onReset() override;
    void notify(std::string const& childChanged) override;
};
}
