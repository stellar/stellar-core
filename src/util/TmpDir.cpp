// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
    while (true)
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

TmpDirMaster::TmpDirMaster(std::string const& root) : mRoot(root)
{
    clean();
    fs::mkdir(root);
}

TmpDirMaster::~TmpDirMaster()
{
    clean();
}

void
TmpDirMaster::clean()
{
    if (fs::exists(mRoot))
    {
        LOG(DEBUG) << "TmpDirMaster cleaning: " << mRoot;
        fs::deltree(mRoot);
    }
}

TmpDir
TmpDirMaster::tmpDir(std::string const& prefix)
{
    return TmpDir(mRoot + "/" + prefix);
}
}
