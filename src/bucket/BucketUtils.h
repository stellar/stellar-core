#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

#define BUCKET_TYPE_ASSERT(BucketT) \
    static_assert(std::is_same_v<BucketT, LiveBucket> || \
                      std::is_same_v<BucketT, HotArchiveBucket>, \
                  "BucketT must be a Bucket type")
}