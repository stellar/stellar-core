// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BucketTestUtils.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "test/test.h"

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
addBatchAndUpdateSnapshot(BucketList& bl, Application& app, LedgerHeader header,
                          std::vector<LedgerEntry> const& initEntries,
                          std::vector<LedgerEntry> const& liveEntries,
                          std::vector<LedgerKey> const& deadEntries)
{
    bl.addBatch(app, header.ledgerSeq, header.ledgerVersion, initEntries,
                liveEntries, deadEntries);
    if (app.getConfig().isUsingBucketListDB())
    {
        app.getBucketManager().getBucketSnapshotManager().updateCurrentSnapshot(
            std::make_unique<BucketListSnapshot>(bl, header));
    }
}

void
for_versions_with_differing_bucket_logic(
    Config const& cfg, std::function<void(Config const&)> const& f)
{
    for_versions(
        {static_cast<uint32_t>(
             Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) -
             1,
         static_cast<uint32_t>(
             Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY),
         static_cast<uint32_t>(Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED)},
        cfg, f);
}

size_t
countEntries(std::shared_ptr<Bucket> bucket)
{
    EntryCounts e(bucket);
    return e.sum();
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
              hexAbbrev(app.getBucketManager().getBucketList().getHash()));
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

EntryCounts::EntryCounts(std::shared_ptr<Bucket> bucket)
{
    BucketInputIterator iter(bucket);
    if (iter.seenMetadata())
    {
        ++nMeta;
    }
    while (iter)
    {
        switch ((*iter).type())
        {
        case INITENTRY:
            ++nInit;
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

void
LedgerManagerForBucketTests::transferLedgerEntriesToBucketList(
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

                LedgerTxn ltxEvictions(ltx);
                if (mApp.getConfig().isUsingBackgroundEviction())
                {
                    mApp.getBucketManager().resolveBackgroundEvictionScan(
                        ltxEvictions, lh.ledgerSeq, keys);
                }
                else
                {
                    mApp.getBucketManager().scanForEvictionLegacy(ltxEvictions,
                                                                  lh.ledgerSeq);
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
                .maybeSnapshotBucketListSize(lh.ledgerSeq, ltx, mApp);
        }

        ltx.getAllEntries(init, live, dead);

        // Add dead entries from ltx to entries that will be added to BucketList
        // so we can test background eviction properly
        if (protocolVersionStartsFrom(initialLedgerVers,
                                      SOROBAN_PROTOCOL_VERSION) &&
            mApp.getConfig().isUsingBackgroundEviction())
        {
            for (auto const& k : dead)
            {
                mTestDeadEntries.emplace_back(k);
            }
        }

        // Use the testing values.
        mApp.getBucketManager().addBatch(mApp, lh, mTestInitEntries,
                                         mTestLiveEntries, mTestDeadEntries);
        mUseTestEntries = false;
    }
    else
    {
        LedgerManagerImpl::transferLedgerEntriesToBucketList(
            ltx, ledgerCloseMeta, lh, initialLedgerVers);
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