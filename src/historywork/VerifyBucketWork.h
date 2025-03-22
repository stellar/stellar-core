// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketUtils.h"
#include "work/Work.h"
#include "xdr/Stellar-types.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class LiveBucketIndex;

class Bucket;

template <typename BucketT> class VerifyBucketWork : public BasicWork
{
    BUCKET_TYPE_ASSERT(BucketT);

    std::string mBucketFile;
    uint256 mHash;
    bool mDone{false};
    std::error_code mEc;
    std::unique_ptr<typename BucketT::IndexT const>& mIndex;
    void spawnVerifier();

    OnFailureCallback mOnFailure;

  public:
    VerifyBucketWork(Application& app, std::string const& bucketFile,
                     uint256 const& hash,
                     std::unique_ptr<typename BucketT::IndexT const>& index,
                     OnFailureCallback failureCb);
    ~VerifyBucketWork() = default;

  protected:
    BasicWork::State onRun() override;
    bool
    onAbort() override
    {
        return true;
    };
    void onFailureRaise() override;
};
}
