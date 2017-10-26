// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GunzipFileWork.h"
#include "util/Fs.h"

namespace stellar
{

GunzipFileWork::GunzipFileWork(Application& app, WorkParent& parent,
                               std::string const& filenameGz, bool keepExisting,
                               size_t maxRetries)
    : RunCommandWork(app, parent, std::string("gunzip-file ") + filenameGz,
                     maxRetries)
    , mFilenameGz(filenameGz)
    , mKeepExisting(keepExisting)
{
    fs::checkGzipSuffix(mFilenameGz);
}

GunzipFileWork::~GunzipFileWork()
{
    clearChildren();
}

void
GunzipFileWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    cmdLine = "gzip -d ";
    if (mKeepExisting)
    {
        cmdLine += "-c ";
        outFile = mFilenameGz.substr(0, mFilenameGz.size() - 3);
    }
    cmdLine += mFilenameGz;
}

void
GunzipFileWork::onReset()
{
    std::string filenameNoGz = mFilenameGz.substr(0, mFilenameGz.size() - 3);
    std::remove(filenameNoGz.c_str());
}
}
