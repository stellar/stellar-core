// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"
#include "xdr/Stellar-types.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class Bucket;

class VerifyBucketWork : public BasicWork
{
    std::map<std::string, std::shared_ptr<Bucket>>& mBuckets;
    std::string mBucketFile;
    uint256 mHash;
    bool mDone{false};
    std::error_code mEc;

    void adoptBucket();
    void spawnVerifier();

    OnFailureCallback mOnFailure;

    medida::Meter& mVerifyBucketSuccess;
    medida::Meter& mVerifyBucketFailure;

  public:
    VerifyBucketWork(Application& app,
                     std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                     std::string const& bucketFile, uint256 const& hash,
                     OnFailureCallback cb);
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
