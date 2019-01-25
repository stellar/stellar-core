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

class RawBucket;
using Bucket = std::shared_ptr<const RawBucket>;

class VerifyBucketWork : public Work
{
    std::map<std::string, Bucket>& mBuckets;
    std::string mBucketFile;
    uint256 mHash;

    medida::Meter& mVerifyBucketSuccess;
    medida::Meter& mVerifyBucketFailure;

  public:
    VerifyBucketWork(Application& app, WorkParent& parent,
                     std::map<std::string, Bucket>& buckets,
                     std::string const& bucketFile, uint256 const& hash);
    ~VerifyBucketWork();
    void onRun() override;
    void onStart() override;
    Work::State onSuccess() override;
    void onFailureRetry() override;
    void onFailureRaise() override;
};
}
