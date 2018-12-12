#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "util/XDRStream.h"
#include <memory>

namespace stellar
{

class Application;

// Class that represents a single apply-bucket-to-database operation in
// progress. Used during history catchup to split up the task of applying
// bucket into scheduler-friendly, bite-sized pieces.

class BucketApplicator
{
    Application& mApp;
    BucketInputIterator mBucketIter;
    size_t mCount{0};

  public:
    BucketApplicator(Application& app, std::shared_ptr<const Bucket> bucket);
    operator bool() const;
    size_t advance();

    size_t pos();
    size_t size() const;
};
}
