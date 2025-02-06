// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/PopulateLedgerCacheWork.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketIndex.h"
#include "bucket/LiveBucketList.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerStateCache.h"
#include "xdrpp/printer.h"

namespace stellar
{
PopulateLedgerCacheWork::PopulateLedgerCacheWork(Application& app)
    : Work(app, "populate-ledger-cache", BasicWork::RETRY_NEVER)
    , mBucketsToProcess{}
    , mBucketToProcessIndex(0)
    , mDeadKeys{}
{
    auto& bm = mApp.getBucketManager();
    auto& bl = bm.getLiveBucketList();
    int counter = 0;
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        for (auto const& bucket : {level.getCurr(), level.getSnap()})
        {
            mBucketsToProcess.push_back(bucket);
            CLOG_DEBUG(Ledger, "Adding bucket {} to mBucketsToProcess[{}]",
                       xdr::xdr_to_string(bucket->getHash()), counter);
            counter++;
        }
    }
}
// TODO possibly refactor BucketApplicator to have modular
// application logic for:
// - selecting entries to apply (e.g. offers, contract entries)
// - applying the entry (e.g. add to ltx or ledgerStateCache)
// It seems the main motivation for BucketApplicator is to limit
// applications to batches of LEDGER_ENTRY_BATCH_COMMIT_SIZE entries.
// No such limit is necessary here, but it would deduplicate
// some shared logic.
BasicWork::State
PopulateLedgerCacheWork::advance()
{
    CLOG_INFO(Ledger, "Added bucket {}/{} to LedgerStateCache",
              mBucketToProcessIndex, mBucketsToProcess.size());
    mBucketToProcessIndex++;
    if (mBucketToProcessIndex >= mBucketsToProcess.size())
    {
        mBucketsToProcess.clear();
        return State::WORK_SUCCESS;
    }
    return State::WORK_RUNNING;
}

BasicWork::State
PopulateLedgerCacheWork::doWork()
{
    auto const& bucket = mBucketsToProcess.at(mBucketToProcessIndex);
    // Hacky way to skip empty buckets
    if (!bucket || bucket->getFilename().empty())
    {
        CLOG_DEBUG(Ledger,
                   "PopulateLedgerCacheWork: Advancing past empty bucket {}",
                   mBucketToProcessIndex);
        return advance();
    }
    auto ledgerStateCache = mApp.getLedgerManager().getLedgerStateCache();
    if (!ledgerStateCache)
    {
        CLOG_DEBUG(Ledger, "LedgerStateCache is not enabled");
        return State::WORK_FAILURE;
    }

    auto cache = ledgerStateCache.value();
    std::streamoff upperBound = 0;
    std::streamoff lowerBound = std::numeric_limits<std::streamoff>::max();
    if (cache->getMode() == LedgerStateCache::Mode::SOROBAN_ONLY)
    {
        // Update the bounds to only iterate over soroban state.
        auto contractEntryRange = bucket->getContractEntryRange();
        if (contractEntryRange)
        {
            lowerBound = std::get<0>(*contractEntryRange);
            upperBound = std::get<1>(*contractEntryRange);
        }
    }

    for (LiveBucketInputIterator iter(bucket); iter && iter.pos() < upperBound;
         ++iter)
    {
        if (iter.pos() < lowerBound)
        {
            iter.seek(lowerBound);
        }
        BucketEntry const& entry = *iter;
        if (entry.type() == LIVEENTRY || entry.type() == INITENTRY)
        {
            auto const& e = entry.liveEntry();
            auto const& k = LedgerEntryKey(e);
            if (!cache->supportedKeyType(k.type()))
            {
                continue;
            }
            // If the key is not in the dead keys set and not already in the
            // cache, add it.
            if (mDeadKeys.find(k) == mDeadKeys.end() && !cache->getEntry(k))
            {
                cache->addEntry(e);
            }
        }
        else if (entry.type() == DEADENTRY)
        {
            if (!cache->getEntry(entry.deadEntry()))
            {
                mDeadKeys.insert(entry.deadEntry());
            }
        }
        else
        {
            releaseAssert(false);
            continue;
        }
    }
    return advance();
}

}