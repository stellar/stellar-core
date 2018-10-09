// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GzipFileWork.h"
#include "util/Fs.h"

namespace stellar
{

GzipFileWork::GzipFileWork(Application& app, std::function<void()> callback,
                           std::string const& filenameNoGz, bool keepExisting)
    : RunCommandWork(app, callback, std::string("gzip-file ") + filenameNoGz)
    , mFilenameNoGz(filenameNoGz)
    , mKeepExisting(keepExisting)
{
    fs::checkNoGzipSuffix(mFilenameNoGz);
}

GzipFileWork::~GzipFileWork()
{
}

void
GzipFileWork::onFailureRetry()
{
    std::string filenameGz = mFilenameNoGz + ".gz";
    std::remove(filenameGz.c_str());
}

void
GzipFileWork::onFailureRaise()
{
    std::string filenameGz = mFilenameNoGz + ".gz";
    std::remove(filenameGz.c_str());
}

RunCommandInfo
GzipFileWork::getCommand()
{
    std::string cmdLine = "gzip ";
    std::string outFile;
    if (mKeepExisting)
    {
        cmdLine += "-c ";
        outFile = mFilenameNoGz + ".gz";
    }
    cmdLine += mFilenameNoGz;

    return RunCommandInfo(cmdLine, outFile);
}
}
