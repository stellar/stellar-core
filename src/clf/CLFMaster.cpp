// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
#include "util/make_unique.h"
#include "util/TmpDir.h"

namespace stellar
{

class CLFMaster::Impl
{
public:
    Application& mApp;
    LedgerHeader mHeader;
    BucketList mBucketList;
    std::unique_ptr<TmpDir> mWorkDir;
    Impl(Application &app)
        : mApp(app)
        , mWorkDir(nullptr)
        {}
};

CLFMaster::CLFMaster(Application& app)
    : mImpl(make_unique<Impl>(app))
{
}

CLFMaster::~CLFMaster()
{
}

std::string const&
CLFMaster::getTmpDir()
{
    if (!mImpl->mWorkDir)
    {
        TmpDir t = mImpl->mApp.getTmpDirMaster().tmpDir("clf");
        mImpl->mWorkDir = make_unique<TmpDir>(std::move(t));
    }
    return mImpl->mWorkDir->getName();
}

LedgerHeader const&
CLFMaster::getHeader()
{
    return mImpl->mHeader;
}

BucketList const&
CLFMaster::getBucketList()
{
    return mImpl->mBucketList;
}

}
