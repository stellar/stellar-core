// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BucketTestUtils.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "test/test.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"
#include <memory>

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
addLiveBatchAndUpdateSnapshot(Application& app, LedgerHeader header,
                              std::vector<LedgerEntry> const& initEntries,
                              std::vector<LedgerEntry> const& liveEntries,
                              std::vector<LedgerKey> const& deadEntries)
{
    auto& liveBl = app.getBucketManager().getLiveBucketList();
    liveBl.addBatch(app, header.ledgerSeq, header.ledgerVersion, initEntries,
                    liveEntries, deadEntries);

    auto liveSnapshot =
        std::make_unique<BucketListSnapshot<LiveBucket>>(liveBl, header);
    auto hotArchiveSnapshot =
        std::make_unique<BucketListSnapshot<HotArchiveBucket>>(
            app.getBucketManager().getHotArchiveBucketList(), header);

    app.getBucketManager().getBucketSnapshotManager().updateCurrentSnapshot(
        std::move(liveSnapshot), std::move(hotArchiveSnapshot));
}

void
addHotArchiveBatchAndUpdateSnapshot(
    Application& app, LedgerHeader header,
    std::vector<LedgerEntry> const& archiveEntries,
    std::vector<LedgerKey> const& restoredEntries)
{
    auto& hotArchiveBl = app.getBucketManager().getHotArchiveBucketList();
    hotArchiveBl.addBatch(app, header.ledgerSeq, header.ledgerVersion,
                          archiveEntries, restoredEntries);
    auto liveSnapshot = std::make_unique<BucketListSnapshot<LiveBucket>>(
        app.getBucketManager().getLiveBucketList(), header);
    auto hotArchiveSnapshot =
        std::make_unique<BucketListSnapshot<HotArchiveBucket>>(hotArchiveBl,
                                                               header);

    app.getBucketManager().getBucketSnapshotManager().updateCurrentSnapshot(
        std::move(liveSnapshot), std::move(hotArchiveSnapshot));
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
         static_cast<uint32_t>(LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED),
         static_cast<uint32_t>(
             LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION)},
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
    while (lm.getLastClosedLedgerNum() < ledgerNum)
    {
        app.getClock().crank(true);
    }
    releaseAssert(lm.getLastClosedLedgerNum() == ledgerNum);
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
        case HOT_ARCHIVE_ARCHIVED:
            ++nInitOrArchived;
            break;
        case HOT_ARCHIVE_LIVE:
            ++nLive;
            break;
        case HOT_ARCHIVE_METAENTRY:
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
LedgerManagerForBucketTests::finalizeLedgerTxnChanges(
    SearchableSnapshotConstPtr lclSnapshot,
    SearchableHotArchiveSnapshotConstPtr lclHotArchiveSnapshot,
    AbstractLedgerTxn& ltx,
    std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
    LedgerHeader lh, uint32_t initialLedgerVers)
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

                auto evictedState =
                    mApp.getBucketManager().resolveBackgroundEvictionScan(
                        lclSnapshot, ltx, keys);
                if (protocolVersionStartsFrom(
                        initialLedgerVers,
                        LiveBucket::
                            FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                {
                    std::vector<LedgerKey> restoredKeys;
                    auto restoredEntriesMap = ltx.getRestoredHotArchiveKeys();
                    for (auto const& [key, entry] : restoredEntriesMap)
                    {
                        // Hot Archive does not track TTLs
                        if (key.type() == CONTRACT_DATA ||
                            key.type() == CONTRACT_CODE)
                        {
                            restoredKeys.emplace_back(key);
                        }
                    }
                    mTestRestoredEntries.insert(mTestRestoredEntries.end(),
                                                restoredKeys.begin(),
                                                restoredKeys.end());
                    mTestArchiveEntries.insert(
                        mTestArchiveEntries.end(),
                        evictedState.archivedEntries.begin(),
                        evictedState.archivedEntries.end());
                    mApp.getBucketManager().addHotArchiveBatch(
                        mApp, lh, mTestArchiveEntries, mTestRestoredEntries);
                }

                if (ledgerCloseMeta)
                {
                    ledgerCloseMeta->populateEvictedEntries(evictedState);
                }
            }
            SorobanNetworkConfig::maybeSnapshotSorobanStateSize(
                lh.ledgerSeq,
                mApp.getLedgerManager().getSorobanInMemoryStateSizeForTesting(),
                ltx, mApp);
        }

        // Load the final Soroban config just before sealing the ltx.
        std::optional<SorobanNetworkConfig> finalSorobanConfig;
        if (protocolVersionStartsFrom(lh.ledgerVersion,
                                      SOROBAN_PROTOCOL_VERSION))
        {
            finalSorobanConfig =
                std::make_optional(SorobanNetworkConfig::loadFromLedger(ltx));
        }
        ltx.getAllEntries(init, live, dead);

        // Add dead entries from ltx to entries that will be added to BucketList
        // so we can test background eviction properly
        if (protocolVersionStartsFrom(initialLedgerVers,
                                      SOROBAN_PROTOCOL_VERSION) ||
            mAlsoAddActualEntries)
        {
            mTestDeadEntries.insert(mTestDeadEntries.end(), dead.begin(),
                                    dead.end());
        }
        if (mAlsoAddActualEntries)
        {
            mTestInitEntries.insert(mTestInitEntries.end(), init.begin(),
                                    init.end());
            // When the actual entries have the same key as test entries, we
            // override the actual entries with the test entries (here we
            // just don't add the actual entries if they're already present).
            for (auto const& liveEntry : live)
            {
                if (std::find_if(mTestLiveEntries.begin(),
                                 mTestLiveEntries.end(),
                                 [liveEntry](auto const& e) {
                                     return LedgerEntryKey(e) ==
                                            LedgerEntryKey(liveEntry);
                                 }) == mTestLiveEntries.end())
                {
                    mTestLiveEntries.push_back(liveEntry);
                }
            }
        }

        // Use the testing values.
        mApplyState.addAnyContractsToModuleCache(lh.ledgerVersion,
                                                 mTestInitEntries);
        mApplyState.addAnyContractsToModuleCache(lh.ledgerVersion,
                                                 mTestLiveEntries);
        mApp.getBucketManager().addLiveBatch(
            mApp, lh, mTestInitEntries, mTestLiveEntries, mTestDeadEntries);

        mApplyState.updateInMemorySorobanState(
            mTestInitEntries, mTestLiveEntries, mTestDeadEntries, lh,
            finalSorobanConfig);

        mUseTestEntries = false;
        mAlsoAddActualEntries = false;
        mTestInitEntries.clear();
        mTestLiveEntries.clear();
        mTestDeadEntries.clear();
        mTestArchiveEntries.clear();
        mTestRestoredEntries.clear();
        mTestDeletedEntries.clear();
    }
    else
    {
        LedgerManagerImpl::finalizeLedgerTxnChanges(
            lclSnapshot, lclHotArchiveSnapshot, ltx, ledgerCloseMeta, lh,
            initialLedgerVers);
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
