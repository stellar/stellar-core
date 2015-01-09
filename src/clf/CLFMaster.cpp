// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
#include "util/make_unique.h"

namespace stellar
{

class CLFMaster::Impl
{
public:
    Application& mApp;
    LedgerHeader mHeader;
    BucketList mBucketList;
    Impl(Application &app)
        : mApp(app)
        {}
};

CLFMaster::CLFMaster(Application& app)
    : mImpl(make_unique<Impl>(app))
{
}

CLFMaster::~CLFMaster()
{
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
