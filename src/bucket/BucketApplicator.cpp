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
#include <fmt/format.h>

namespace stellar
{

BucketApplicator::BucketApplicator(Application& app,
                                   uint32_t maxProtocolVersion,
                                   std::shared_ptr<const Bucket> bucket)
    : mApp(app), mMaxProtocolVersion(maxProtocolVersion), mBucketIter(bucket)
{
    auto protocolVersion = mBucketIter.getMetadata().ledgerVersion;
    if (protocolVersion > mMaxProtocolVersion)
    {
        throw std::runtime_error(fmt::format(
            "bucket protocol version {} exceeds maxProtocolVersion {}",
            protocolVersion, mMaxProtocolVersion));
    }
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

    auto& root = mApp.getLedgerTxnRoot();
    LedgerTxn ltx(root, false);
    for (; mBucketIter; ++mBucketIter)
    {
        BucketEntry const& e = *mBucketIter;
        Bucket::checkProtocolLegality(e, mMaxProtocolVersion);
        counters.mark(e);
        if (e.type() == LIVEENTRY || e.type() == INITENTRY)
        {
            ltx.createOrUpdateWithoutLoading(e.liveEntry());
        }
        else
        {
            if (e.type() != DEADENTRY)
            {
                throw std::runtime_error(
                    "Malformed bucket: unexpected non-INIT/LIVE/DEAD entry.");
            }
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
    mClaimableBalanceUpsert = 0;
    mClaimableBalanceDelete = 0;
}

void
BucketApplicator::Counters::getRates(VirtualClock::time_point now,
                                     uint64_t& au_sec, uint64_t& ad_sec,
                                     uint64_t& tu_sec, uint64_t& td_sec,
                                     uint64_t& ou_sec, uint64_t& od_sec,
                                     uint64_t& du_sec, uint64_t& dd_sec,
                                     uint64_t& cu_sec, uint64_t& cd_sec,
                                     uint64_t& T_sec, uint64_t& total)
{
    VirtualClock::duration dur = now - mStarted;
    auto usec = std::chrono::duration_cast<std::chrono::microseconds>(dur);
    uint64_t usecs = usec.count() + 1;
    total = mAccountUpsert + mAccountDelete + mTrustLineUpsert +
            mTrustLineDelete + mOfferUpsert + mOfferDelete + mDataUpsert +
            mDataDelete + mClaimableBalanceUpsert + mClaimableBalanceDelete;
    au_sec = (mAccountUpsert * 1000000) / usecs;
    ad_sec = (mAccountDelete * 1000000) / usecs;
    tu_sec = (mTrustLineUpsert * 1000000) / usecs;
    td_sec = (mTrustLineDelete * 1000000) / usecs;
    ou_sec = (mOfferUpsert * 1000000) / usecs;
    od_sec = (mOfferDelete * 1000000) / usecs;
    du_sec = (mDataUpsert * 1000000) / usecs;
    dd_sec = (mDataDelete * 1000000) / usecs;
    cu_sec = (mClaimableBalanceUpsert * 1000000) / usecs;
    cd_sec = (mClaimableBalanceDelete * 1000000) / usecs;
    T_sec = (total * 1000000) / usecs;
}

void
BucketApplicator::Counters::logInfo(std::string const& bucketName,
                                    uint32_t level,
                                    VirtualClock::time_point now)
{
    uint64_t au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec, dd_sec,
        cu_sec, cd_sec, T_sec, total;
    getRates(now, au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec,
             dd_sec, cu_sec, cd_sec, T_sec, total);
    CLOG_INFO(Bucket,
              "Apply-rates for {}-entry bucket {}.{} au:{} ad:{} tu:{} td:{} "
              "ou:{} od:{} du:{} dd:{} cu:{} cd:{} T:{}",
              total, level, bucketName, au_sec, ad_sec, tu_sec, td_sec, ou_sec,
              od_sec, du_sec, dd_sec, cu_sec, cd_sec, T_sec);
    CLOG_INFO(Bucket,
              "Entry-counts for {}-entry bucket {}.{} au:{} ad:{} tu:{} td:{} "
              "ou:{} od:{} du:{} dd:{} cu:{} cd:{}",
              total, level, bucketName, mAccountUpsert, mAccountDelete,
              mTrustLineUpsert, mTrustLineDelete, mOfferUpsert, mOfferDelete,
              mDataUpsert, mDataDelete, mClaimableBalanceUpsert,
              mClaimableBalanceDelete);
}

void
BucketApplicator::Counters::logDebug(std::string const& bucketName,
                                     uint32_t level,
                                     VirtualClock::time_point now)
{
    uint64_t au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec, dd_sec,
        cu_sec, cd_sec, T_sec, total;
    getRates(now, au_sec, ad_sec, tu_sec, td_sec, ou_sec, od_sec, du_sec,
             dd_sec, cu_sec, cd_sec, T_sec, total);
    CLOG_DEBUG(Bucket,
               "Apply-rates for {}-entry bucket {}.{} au:{} ad:{} tu:{} td:{} "
               "ou:{} od:{} du:{} dd:{} cu:{} cd:{} T:{}",
               total, level, bucketName, au_sec, ad_sec, tu_sec, td_sec, ou_sec,
               od_sec, du_sec, dd_sec, cu_sec, cd_sec, T_sec);
}

void
BucketApplicator::Counters::mark(BucketEntry const& e)
{
    if (e.type() == LIVEENTRY || e.type() == INITENTRY)
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
        case CLAIMABLE_BALANCE:
            ++mClaimableBalanceUpsert;
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
        case CLAIMABLE_BALANCE:
            ++mClaimableBalanceDelete;
            break;
        }
    }
}
}
