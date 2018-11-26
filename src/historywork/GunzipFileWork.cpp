// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GunzipFileWork.h"
#include "util/Fs.h"

namespace stellar
{

GunzipFileWork::GunzipFileWork(Application& app, std::string const& filenameGz,
                               bool keepExisting, size_t maxRetries)
    : RunCommandWork(app, std::string("gunzip-file ") + filenameGz, maxRetries)
    , mFilenameGz(filenameGz)
    , mKeepExisting(keepExisting)
{
    fs::checkGzipSuffix(mFilenameGz);
}

CommandInfo
GunzipFileWork::getCommand()
{
    std::string cmdLine, outFile;
    cmdLine = "gzip -d ";
    if (mKeepExisting)
    {
        cmdLine += "-c ";
        outFile = mFilenameGz.substr(0, mFilenameGz.size() - 3);
    }
    cmdLine += mFilenameGz;
    return CommandInfo{cmdLine, outFile};
}

void
GunzipFileWork::onReset()
{
    std::string filenameNoGz = mFilenameGz.substr(0, mFilenameGz.size() - 3);
    std::remove(filenameNoGz.c_str());
}
}
