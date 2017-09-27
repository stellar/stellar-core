#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <string>
#include <vector>

namespace stellar
{

class PublishQueueBuckets
{
  public:
    void addBuckets(std::vector<std::string> const& buckets);
    void addBucket(std::string const& bucket);

    void removeBuckets(std::vector<std::string> const& buckets);
    void removeBucket(std::string const& bucket);

    std::map<std::string, int> const&
    map() const
    {
        return mBucketUsage;
    }

  private:
    std::map<std::string, int> mBucketUsage;
};
}
