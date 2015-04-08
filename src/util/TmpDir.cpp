// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/TmpDir.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "crypto/Random.h"
#include "crypto/Hex.h"
#include "util/Fs.h"

namespace stellar
{

TmpDir::TmpDir(std::string const& prefix)
{
    size_t attempts = 0;
    for (;;)
    {
        std::string hex = binToHex(randomBytes(8));
        std::string name = prefix + "-" + hex;
        if (fs::mkdir(name))
        {
            mPath = make_unique<std::string>(name);
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
    if (mPath)
    {
        fs::deltree(*mPath);
        LOG(DEBUG) << "TmpDir deleted: " << *mPath;
        mPath.reset();
    }
}

TmpDirManager::TmpDirManager(std::string const& root) : mRoot(root)
{
    clean();
    fs::mkdir(root);
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
