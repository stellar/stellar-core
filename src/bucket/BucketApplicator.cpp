// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketApplicator.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
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
                                   uint32_t minProtocolVersionSeen,
                                   uint32_t level,
                                   std::shared_ptr<Bucket const> bucket,
                                   std::function<bool(LedgerEntryType)> filter)
    : mApp(app)
    , mMaxProtocolVersion(maxProtocolVersion)
    , mMinProtocolVersionSeen(minProtocolVersionSeen)
    , mLevel(level)
    , mBucketIter(bucket)
    , mEntryTypeFilter(filter)
{
    auto protocolVersion = mBucketIter.getMetadata().ledgerVersion;
    if (protocolVersion > mMaxProtocolVersion)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING(
                "bucket protocol version {:d} exceeds maxProtocolVersion {:d}"),
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

static bool
shouldApplyEntry(std::function<bool(LedgerEntryType)> const& filter,
                 BucketEntry const& e)
{
    if (e.type() == LIVEENTRY || e.type() == INITENTRY)
    {
        return filter(e.liveEntry().data.type());
    }

    if (e.type() != DEADENTRY)
    {
        throw std::runtime_error(
            "Malformed bucket: unexpected non-INIT/LIVE/DEAD entry.");
    }
    return filter(e.deadEntry().type());
}

size_t
BucketApplicator::advance(BucketApplicator::Counters& counters)
{
    size_t count = 0;

    auto& root = mApp.getLedgerTxnRoot();
    AbstractLedgerTxn* ltx;
    std::unique_ptr<LedgerTxn> innerLtx;

    // when running in memory mode, make changes to the in memory ledger
    // directly instead of creating a temporary inner LedgerTxn
    // as "advance" commits changes during each step this does not introduce any
    // new failure mode
    if (mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        ltx = static_cast<AbstractLedgerTxn*>(&root);
    }
    else
    {
        innerLtx = std::make_unique<LedgerTxn>(root, false);
        ltx = innerLtx.get();
        ltx->prepareNewObjects(LEDGER_ENTRY_BATCH_COMMIT_SIZE);
    }

    for (; mBucketIter; ++mBucketIter)
    {
        BucketEntry const& e = *mBucketIter;
        Bucket::checkProtocolLegality(e, mMaxProtocolVersion);

        if (shouldApplyEntry(mEntryTypeFilter, e))
        {
            counters.mark(e);

            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                // The last level can have live entries, but at that point we
                // know that they are actually init entries because the earliest
                // state of all entries is init, so we mark them as such here
                if (mLevel == BucketList::kNumLevels - 1 &&
                    e.type() == LIVEENTRY)
                {
                    ltx->createWithoutLoading(e.liveEntry());
                }
                else if (
                    protocolVersionIsBefore(
                        mMinProtocolVersionSeen,
                        Bucket::
                            FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
                {
                    // Prior to protocol 11, INITENTRY didn't exist, so we need
                    // to check ltx to see if this is an update or a create
                    auto key = InternalLedgerEntry(e.liveEntry()).toKey();
                    if (ltx->getNewestVersion(key))
                    {
                        ltx->updateWithoutLoading(e.liveEntry());
                    }
                    else
                    {
                        ltx->createWithoutLoading(e.liveEntry());
                    }
                }
                else
                {
                    if (e.type() == LIVEENTRY)
                    {
                        ltx->updateWithoutLoading(e.liveEntry());
                    }
                    else
                    {
                        ltx->createWithoutLoading(e.liveEntry());
                    }
                }
            }
            else
            {
                if (protocolVersionIsBefore(
                        mMinProtocolVersionSeen,
                        Bucket::
                            FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
                {
                    // Prior to protocol 11, DEAD entries could exist
                    // without LIVE entries in between
                    if (ltx->getNewestVersion(e.deadEntry()))
                    {
                        ltx->eraseWithoutLoading(e.deadEntry());
                    }
                }
                else
                {
                    ltx->eraseWithoutLoading(e.deadEntry());
                }
            }

            if ((++count > LEDGER_ENTRY_BATCH_COMMIT_SIZE))
            {
                ++mBucketIter;
                break;
            }
        }
    }
    if (innerLtx)
    {
        ltx->commit();
    }

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
    mUpserted.fill(0);
    mDeleted.fill(0);
}

void
BucketApplicator::Counters::getRates(VirtualClock::time_point now,
                                     CounterArray& upserted_sec,
                                     CounterArray& deleted_sec, uint64_t& T_sec,
                                     uint64_t& total)
{
    VirtualClock::duration dur = now - mStarted;
    auto usec = std::chrono::duration_cast<std::chrono::microseconds>(dur);
    uint64_t usecs = usec.count() + 1;
    total = 0;
    for (auto i = 0; i < std::tuple_size<CounterArray>::value; ++i)
    {
        upserted_sec[i] = (mUpserted[i] * 1000000) / usecs;
        deleted_sec[i] = (mDeleted[i] * 1000000) / usecs;
        total += mUpserted[i];
        total += mDeleted[i];
    }

    T_sec = (total * 1000000) / usecs;
}

std::string
BucketApplicator::Counters::logStr(uint64_t total, uint64_t level,
                                   std::string const& bucketName,
                                   CounterArray const& upserted,
                                   CounterArray const& deleted)
{
    return fmt::format("for {}-entry bucket {}.{} au:{} ad:{} tu:{} td:{} "
                       "ou:{} od:{} du:{} dd:{} cu:{} cd:{} lu:{} ld:{} "
                       "cdu:{} cdd:{} ccu:{} ccd:{} csu:{} eeu:{} eed:{}",
                       total, level, bucketName, upserted[0], deleted[0],
                       upserted[1], deleted[1], upserted[2], deleted[2],
                       upserted[3], deleted[3], upserted[4], deleted[4],
                       upserted[5], deleted[5], upserted[6], deleted[6],
                       upserted[7], deleted[7], upserted[8], deleted[8]);
}

void
BucketApplicator::Counters::logInfo(std::string const& bucketName,
                                    uint32_t level,
                                    VirtualClock::time_point now)
{
    uint64_t T_sec, total;
    CounterArray upserted_sec, deleted_sec;
    getRates(now, upserted_sec, deleted_sec, T_sec, total);
    CLOG_INFO(Bucket, "Apply-rates {} T:{}",
              logStr(total, level, bucketName, upserted_sec, deleted_sec),
              T_sec);
    CLOG_INFO(Bucket, "Entry-counts {}",
              logStr(total, level, bucketName, mUpserted, mDeleted));
}

void
BucketApplicator::Counters::logDebug(std::string const& bucketName,
                                     uint32_t level,
                                     VirtualClock::time_point now)
{
    uint64_t T_sec, total;
    CounterArray upserted_sec, deleted_sec;
    getRates(now, upserted_sec, deleted_sec, T_sec, total);
    CLOG_DEBUG(Bucket, "Apply-rates {} T:{}",
               logStr(total, level, bucketName, upserted_sec, deleted_sec),
               T_sec);
}

void
BucketApplicator::Counters::mark(BucketEntry const& e)
{
    if (e.type() == LIVEENTRY || e.type() == INITENTRY)
    {
        ++mUpserted[e.liveEntry().data.type()];
    }
    else
    {
        ++mDeleted[e.deadEntry().type()];
    }
}
}
