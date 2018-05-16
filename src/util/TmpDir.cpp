// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/TmpDir.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Fs.h"
#include "util/Logging.h"

namespace stellar
{

TmpDir::TmpDir(std::string const& prefix)
{
    size_t attempts = 0;
    for (;;)
    {
        std::string hex = binToHex(randomBytes(8));
        std::string name = prefix + "-" + hex;
        if (fs::mkpath(name))
        {
            mPath = std::make_unique<std::string>(name);
            break;
        }
        if (++attempts > 100)
        {
            throw std::runtime_error("failed to create TmpDir");
        }
    }
}

TmpDir::TmpDir(TmpDir&& other) : mPath(std::move(other.mPath))
{
}

std::string const&
TmpDir::getName() const
{
    return *mPath;
}

TmpDir::~TmpDir()
{
    if (!mPath)
    {
        return;
    }

    try
    {
        fs::deltree(*mPath);
        LOG(DEBUG) << "TmpDir deleted: " << *mPath;
    }
    catch (std::runtime_error& e)
    {
        LOG(ERROR) << "Failed to delete TmpDir: " << *mPath
                   << ", because: " << e.what();
    }

    mPath.reset();
}

TmpDirManager::TmpDirManager(std::string const& root) : mRoot(root)
{
    clean();
    fs::mkpath(root);
}

TmpDirManager::~TmpDirManager()
{
    clean();
}

void
TmpDirManager::clean()
{
    if (fs::exists(mRoot))
    {
        LOG(DEBUG) << "TmpDirManager cleaning: " << mRoot;
        fs::deltree(mRoot);
    }
}

TmpDir
TmpDirManager::tmpDir(std::string const& prefix)
{
    return TmpDir(mRoot + "/" + prefix);
}
}
