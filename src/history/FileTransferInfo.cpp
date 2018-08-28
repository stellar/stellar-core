// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "FileTransferInfo.h"

namespace stellar
{
char const* HISTORY_FILE_TYPE_BUCKET = "bucket";
char const* HISTORY_FILE_TYPE_LEDGER = "ledger";
char const* HISTORY_FILE_TYPE_TRANSACTIONS = "transactions";
char const* HISTORY_FILE_TYPE_RESULTS = "results";
char const* HISTORY_FILE_TYPE_SCP = "scp";

std::string
FileTransferInfo::getLocalDir(TmpDir const& localRoot) const
{
    auto localDir = localRoot.getName();
    localDir += "/" + fs::remoteDir(mType, mHexDigits);
    int retries = 5;
    // Similarly to TmpDir, retry in case there were
    // OS-related errors (e.g. out of memory) or race conditions
    while (!fs::mkpath(localDir))
    {
        if (--retries == 0)
        {
            throw std::runtime_error("Unable to make a path " + localDir);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return localDir;
}
}
