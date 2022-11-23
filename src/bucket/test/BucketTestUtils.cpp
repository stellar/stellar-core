// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BucketTestUtils.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
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
    app.getHerder().externalizeValue(TxSetFrame::makeEmpty(lcl), ledgerNum,
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
    AbstractLedgerTxn& ltx, uint32_t ledgerSeq, uint32_t ledgerVers)
{
    if (mUseTestEntries)
    {
        // Seal the ltx but throw its entries away.
        std::vector<LedgerEntry> init, live;
        std::vector<LedgerKey> dead;
        ltx.getAllEntries(init, live, dead);
        // Use the testing values.
        mApp.getBucketManager().addBatch(mApp, ledgerSeq, ledgerVers,
                                         mTestInitEntries, mTestLiveEntries,
                                         mTestDeadEntries);
        mUseTestEntries = false;
    }
    else
    {
        LedgerManagerImpl::transferLedgerEntriesToBucketList(ltx, ledgerSeq,
                                                             ledgerVers);
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