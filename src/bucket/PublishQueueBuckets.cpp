// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/PublishQueueBuckets.h"

namespace stellar
{

void
PublishQueueBuckets::addBuckets(std::vector<std::string> const& buckets)
{
    for (auto const& bucket : buckets)
    {
        addBucket(bucket);
    }
}

void
PublishQueueBuckets::addBucket(std::string const& bucket)
{
    mBucketUsage[bucket]++;
}

void
PublishQueueBuckets::removeBuckets(std::vector<std::string> const& buckets)
{
    for (auto const& bucket : buckets)
    {
        removeBucket(bucket);
    }
}

void
PublishQueueBuckets::removeBucket(std::string const& bucket)
{
    auto it = mBucketUsage.find(bucket);
    if (it == std::end(mBucketUsage))
    {
        return;
    }

    it->second--;
    if (it->second == 0)
    {
        mBucketUsage.erase(it);
    }
}
}
