// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/Bucket.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{

BucketApplicator::BucketApplicator(Application& app,
                                   std::shared_ptr<const Bucket> bucket)
    : mApp(app), mBucketIter(bucket)
{
}

BucketApplicator::operator bool() const
{
    return (bool)mBucketIter;
}

size_t
BucketApplicator::pos()
{
    return mBucketIter.pos();
}

size_t
BucketApplicator::size() const
{
    return mBucketIter.size();
}

size_t
BucketApplicator::advance(BucketApplicator::Counters& counters)
{
    size_t count = 0;

    LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
    for (; mBucketIter; ++mBucketIter)
    {
        BucketEntry const& e = *mBucketIter;
        counters.mark(e);
        if (e.type() == LIVEENTRY)
        {
            ltx.createOrUpdateWithoutLoading(e.liveEntry());
        }
        else
        {
            ltx.eraseWithoutLoading(e.deadEntry());
        }

        if ((++count > LEDGER_ENTRY_BATCH_COMMIT_SIZE))
        {
            break;
        }
    }
    ltx.commit();

    mCount += count;
    return count;
}

BucketApplicator::Counters::Counters(VirtualClock::time_point now)
{
    reset(now);
}

void
BucketApplicator::Counters::reset(VirtualClock::time_point now)
{
    mStarted = now;
    mAccountUpsert = 0;
    mAccountDelete = 0;
    mTrustLineUpsert = 0;
    mTrustLineDelete = 0;
    mOfferUpsert = 0;
    mOfferDelete = 0;
    mDataUpsert = 0;
    mDataDelete = 0;
}

void
BucketApplicator::Counters::getRates(VirtualClock::time_point now,
                                     uint64_t& au_sec, uint64_t& ad_sec,
                                     uint64_t& tu_sec, uint64_t& td_sec,
                                     uint64_t& ou_sec, uint64_t& od_sec,
                                     uint64_t& du_sec, uint64_t& dd_sec,
                                     uint64_t& T_sec, uint64_t& total)
{
    VirtualClock::duration dur = now - mStarted;
    auto usec = std::chrono::duration_cast<std::chrono::microseconds>(dur);
    uint64_t usecs = usec.count() + 1;
    total = mAccountUpsert + mAccountDelete + mTrustLineUpsert +
            mTrustLineDelete + mOfferUpsert + mOfferDelete + mDataUpsert +
            mDataDelete;
    au_sec = (mAccountUpsert * 1000000) / usecs;
    ad_sec = (mAccountDelete * 1000000) / usecs;
    tu_sec = (mTrustLineUpsert * 1000000) / usecs;
    td_sec = (mTrustLineDelete * 1000000) / usecs;
    ou_sec = (mOfferUpsert * 1000000) / usecs;
    od_sec = (mOfferDelete * 1000000) / usecs;
    du_sec = (mDataUpsert * 1000000) / usecs;
    dd_sec = (mDataDelete * 1000000) / usecs;
    T_sec = (total * 1000000) / usecs;
}

void
BucketApplicator::Counters::logInfo(std::string const& bucketName,
                                    uint32_t level,
                                    VirtualClock::time_point now)
{
    uint64_t au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec, dd_sec,
        T_sec, total;
    getRates(now, au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec,
             dd_sec, T_sec, total);
    CLOG(INFO, "Bucket") << "Apply-rates for " << total << "-entry bucket "
                         << level << "." << bucketName << " au:" << au_sec
                         << " ad:" << ad_sec << " tu:" << tu_sec
                         << " td:" << td_sec << " ou:" << ou_sec
                         << " od:" << od_sec << " du:" << du_sec
                         << " dd:" << dd_sec << " T:" << T_sec;
    CLOG(INFO, "Bucket") << "Entry-counts for " << total << "-entry bucket "
                         << level << "." << bucketName
                         << " au:" << mAccountUpsert << " ad:" << mAccountDelete
                         << " tu:" << mTrustLineUpsert
                         << " td:" << mTrustLineDelete << " ou:" << mOfferUpsert
                         << " od:" << mOfferDelete << " du:" << mDataUpsert
                         << " dd:" << mDataDelete;
}

void
BucketApplicator::Counters::logDebug(std::string const& bucketName,
                                     uint32_t level,
                                     VirtualClock::time_point now)
{
    uint64_t au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec, dd_sec,
        T_sec, total;
    getRates(now, au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec,
             dd_sec, T_sec, total);
    CLOG(DEBUG, "Bucket") << "Apply-rates for " << total << "-entry bucket "
                          << level << "." << bucketName << " au:" << au_sec
                          << " ad:" << ad_sec << " tu:" << tu_sec
                          << " td:" << td_sec << " ou:" << ou_sec
                          << " od:" << od_sec << " du:" << du_sec
                          << " dd:" << dd_sec << " T:" << T_sec;
}

void
BucketApplicator::Counters::mark(BucketEntry const& e)
{
    if (e.type() == LIVEENTRY)
    {
        switch (e.liveEntry().data.type())
        {
        case ACCOUNT:
            ++mAccountUpsert;
            break;
        case TRUSTLINE:
            ++mTrustLineUpsert;
            break;
        case OFFER:
            ++mOfferUpsert;
            break;
        case DATA:
            ++mDataUpsert;
            break;
        }
    }
    else
    {
        switch (e.deadEntry().type())
        {
        case ACCOUNT:
            ++mAccountDelete;
            break;
        case TRUSTLINE:
            ++mTrustLineDelete;
            break;
        case OFFER:
            ++mOfferDelete;
            break;
        case DATA:
            ++mDataDelete;
            break;
        }
    }
}
}
