#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "util/Timer.h"
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
    uint32_t mMaxProtocolVersion;
    BucketInputIterator mBucketIter;
    size_t mCount{0};

  public:
    class Counters
    {
        // We avoid using medida metrics here here because BucketApplicator
        // activity is typically a rare thing that happens only during catchup
        // and we don't want to pollute the regular operational metrics
        // interface with these counts.
        VirtualClock::time_point mStarted;
        uint64_t mAccountUpsert;
        uint64_t mAccountDelete;
        uint64_t mTrustLineUpsert;
        uint64_t mTrustLineDelete;
        uint64_t mOfferUpsert;
        uint64_t mOfferDelete;
        uint64_t mDataUpsert;
        uint64_t mDataDelete;
        uint64_t mClaimableBalanceUpsert;
        uint64_t mClaimableBalanceDelete;
        void getRates(VirtualClock::time_point now, uint64_t& au_sec,
                      uint64_t& ad_sec, uint64_t& tu_sec, uint64_t& td_sec,
                      uint64_t& ou_sec, uint64_t& od_sec, uint64_t& du_sec,
                      uint64_t& dd_sec, uint64_t& cu_sec, uint64_t& cd_sec,
                      uint64_t& T_sec, uint64_t& total);

      public:
        Counters(VirtualClock::time_point now);
        void reset(VirtualClock::time_point now);
        void mark(BucketEntry const& e);
        void logInfo(std::string const& bucketName, uint32_t level,
                     VirtualClock::time_point now);
        void logDebug(std::string const& bucketName, uint32_t level,
                      VirtualClock::time_point now);
    };

    BucketApplicator(Application& app, uint32_t maxProtocolVersion,
                     std::shared_ptr<const Bucket> bucket);
    operator bool() const;
    size_t advance(Counters& counters);

    size_t pos();
    size_t size() const;
};
}
