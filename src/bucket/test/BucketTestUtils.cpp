// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BucketTestUtils.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "test/test.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{
namespace BucketTestUtils
{

uint32_t
getAppLedgerVersion(Application& app)
{
    auto const& lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    return lcl.header.ledgerVersion;
}

uint32_t
getAppLedgerVersion(Application::pointer app)
{
    return getAppLedgerVersion(*app);
}

void
for_versions_with_differing_bucket_logic(
    Config const& cfg, std::function<void(Config const&)> const& f)
{
    for_versions(
        {static_cast<uint32_t>(
             LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) -
             1,
         static_cast<uint32_t>(
             LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY),
         static_cast<uint32_t>(LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED)},
        cfg, f);
}

Hash
closeLedger(Application& app, std::optional<SecretKey> skToSignValue,
            xdr::xvector<UpgradeType, 6> upgrades)
{
    auto& lm = app.getLedgerManager();
    auto lcl = lm.getLastClosedLedgerHeader();
    uint32_t ledgerNum = lcl.header.ledgerSeq + 1;
    CLOG_INFO(Bucket, "Artificially closing ledger {} with lcl={}, buckets={}",
              ledgerNum, hexAbbrev(lcl.hash),
              hexAbbrev(app.getBucketManager().getLiveBucketList().getHash()));
    app.getHerder().externalizeValue(TxSetXDRFrame::makeEmpty(lcl), ledgerNum,
                                     lcl.header.scpValue.closeTime, upgrades,
                                     skToSignValue);
    return lm.getLastClosedLedgerHeader().hash;
}

Hash
closeLedger(Application& app)
{
    return closeLedger(app, std::nullopt);
}

template <>
EntryCounts<LiveBucket>::EntryCounts(std::shared_ptr<LiveBucket> bucket)
{
    LiveBucketInputIterator iter(bucket);
    if (iter.seenMetadata())
    {
        ++nMeta;
    }
    while (iter)
    {
        switch ((*iter).type())
        {
        case INITENTRY:
            ++nInitOrArchived;
            break;
        case LIVEENTRY:
            ++nLive;
            break;
        case DEADENTRY:
            ++nDead;
            break;
        case METAENTRY:
            // This should never happen: only the first record can be METAENTRY
            // and it is counted above.
            abort();
        }
        ++iter;
    }
}

template <>
EntryCounts<HotArchiveBucket>::EntryCounts(
    std::shared_ptr<HotArchiveBucket> bucket)
{
    HotArchiveBucketInputIterator iter(bucket);
    if (iter.seenMetadata())
    {
        ++nMeta;
    }
    while (iter)
    {
        switch ((*iter).type())
        {
        case HA_ARCHIVED:
            ++nInitOrArchived;
            break;
        case HA_LIVE:
            ++nLive;
            break;
        case HA_DELETED:
            ++nDead;
            break;
        case HA_METAENTRY:
            // This should never happen: only the first record can be METAENTRY
            // and it is counted above.
            abort();
        }
        ++iter;
    }
}

template <class BucketT>
size_t
countEntries(std::shared_ptr<BucketT> bucket)
{
    EntryCounts e(bucket);
    return e.sum();
}

template size_t countEntries(std::shared_ptr<LiveBucket> bucket);
template size_t countEntries(std::shared_ptr<HotArchiveBucket> bucket);

void
LedgerManagerForBucketTests::transferLedgerEntriesToBucketList(
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    uint32_t ledgerSeq, uint32_t currLedgerVers, uint32_t initialLedgerVers)
{
    if (mUseTestEntries)
    {
        // Seal the ltx but throw its entries away.
        std::vector<LedgerEntry> init, live;
        std::vector<LedgerKey> dead;

        // Any V20 features must be behind initialLedgerVers check, see comment
        // in LedgerManagerImpl::ledgerClosed
        if (protocolVersionStartsFrom(initialLedgerVers,
                                      SOROBAN_PROTOCOL_VERSION))
        {
            {
                // LedgerManagerForBucketTests does not modify entries via the
                // ltx subsystem, so replicate the behavior of
                // ltx.getAllTTLKeysWithoutSealing() here
                LedgerKeySet keys;
                for (auto const& le : mTestInitEntries)
                {
                    if (le.data.type() == TTL)
                    {
                        keys.emplace(LedgerEntryKey(le));
                    }
                }

                for (auto const& le : mTestLiveEntries)
                {
                    if (le.data.type() == TTL)
                    {
                        keys.emplace(LedgerEntryKey(le));
                    }
                }

                for (auto const& key : mTestDeadEntries)
                {
                    if (key.type() == TTL)
                    {
                        keys.emplace(key);
                    }
                }

                LedgerTxn ltxEvictions(ltx);
                if (mApp.getConfig().EXPERIMENTAL_BACKGROUND_EVICTION_SCAN)
                {
                    mApp.getBucketManager().resolveBackgroundEvictionScan(
                        ltxEvictions, ledgerSeq, keys);
                }
                else
                {
                    mApp.getBucketManager().scanForEvictionLegacy(ltxEvictions,
                                                                  ledgerSeq);
                }

                if (ledgerCloseMeta)
                {
                    ledgerCloseMeta->populateEvictedEntries(
                        ltxEvictions.getChanges());
                }
                ltxEvictions.commit();
            }
            mApp.getLedgerManager()
                .getMutableSorobanNetworkConfig()
                .maybeSnapshotBucketListSize(ledgerSeq, ltx, mApp);
        }

        ltx.getAllEntries(init, live, dead);

        // Add dead entries from ltx to entries that will be added to BucketList
        // so we can test background eviction properly
        if (mApp.getConfig().EXPERIMENTAL_BACKGROUND_EVICTION_SCAN)
        {
            for (auto const& k : dead)
            {
                mTestDeadEntries.emplace_back(k);
            }
        }

        // Use the testing values.
        mApp.getBucketManager().addLiveBatch(mApp, ledgerSeq, currLedgerVers,
                                             mTestInitEntries, mTestLiveEntries,
                                             mTestDeadEntries);
        mUseTestEntries = false;
    }
    else
    {
        LedgerManagerImpl::transferLedgerEntriesToBucketList(
            ltx, ledgerCloseMeta, ledgerSeq, currLedgerVers, initialLedgerVers);
    }
}

LedgerManagerForBucketTests&
BucketTestApplication::getLedgerManager()
{
    auto& lm = ApplicationImpl::getLedgerManager();
    return static_cast<LedgerManagerForBucketTests&>(lm);
}
}
}