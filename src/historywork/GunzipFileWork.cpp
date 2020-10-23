// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GunzipFileWork.h"
#include "util/Fs.h"

namespace
{
std::string
withoutGz(std::string const& withGz)
{
    stellar::fs::checkGzipSuffix(withGz);
    return withGz.substr(0, withGz.size() - 3);
}
}

namespace stellar
{

GunzipFileWork::GunzipFileWork(Application& app, std::string const& filenameGz,
                               bool keepExisting, size_t maxRetries)
    : RunCommandWork(app, std::string("gunzip-file ") + filenameGz, maxRetries)
    , mFilenameGz(filenameGz)
    , mFilenameNoGz(withoutGz(filenameGz))
    , mKeepExisting(keepExisting)
{
}

CommandInfo
GunzipFileWork::getCommand()
{
    std::string cmdLine, outFile;
    cmdLine = "gzip -d ";
    if (mKeepExisting)
    {
        cmdLine += "-c ";
        outFile = mFilenameNoGz;
    }
    cmdLine += mFilenameGz;
    return CommandInfo{cmdLine, outFile};
}

std::vector<std::string>
GunzipFileWork::getFilesToFlush() const
{
    return {mFilenameNoGz};
}

void
GunzipFileWork::onReset()
{
    std::remove(mFilenameNoGz.c_str());
}
}
