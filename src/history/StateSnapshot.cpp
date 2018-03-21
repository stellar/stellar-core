// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/StateSnapshot.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "herder/HerderPersistence.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerHeaderFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/SociNoWarnings.h"
#include "util/XDRStream.h"
#include "util/make_unique.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

StateSnapshot::StateSnapshot(Application& app, HistoryArchiveState const& state)
    : mApp(app)
    , mLocalState(state)
    , mSnapDir(app.getTmpDirManager().tmpDir("snapshot"))
    , mLedgerSnapFile(std::make_shared<FileTransferInfo>(
          mSnapDir, HISTORY_FILE_TYPE_LEDGER, mLocalState.currentLedger))

    , mTransactionSnapFile(std::make_shared<FileTransferInfo>(
          mSnapDir, HISTORY_FILE_TYPE_TRANSACTIONS, mLocalState.currentLedger))

    , mTransactionResultSnapFile(std::make_shared<FileTransferInfo>(
          mSnapDir, HISTORY_FILE_TYPE_RESULTS, mLocalState.currentLedger))

    , mSCPHistorySnapFile(std::make_shared<FileTransferInfo>(
          mSnapDir, HISTORY_FILE_TYPE_SCP, mLocalState.currentLedger))

{
    makeLive();
}

void
StateSnapshot::makeLive()
{
    for (uint32_t i = 0;
         i < static_cast<uint32>(mLocalState.currentBuckets.size()); i++)
    {
        auto& hb = mLocalState.currentBuckets[i];
        if (hb.next.hasHashes() && !hb.next.isLive())
        {
            hb.next.makeLive(mApp, BucketList::keepDeadEntries(i));
        }
    }
}

bool
StateSnapshot::writeHistoryBlocks() const
{
    std::unique_ptr<soci::session> snapSess(
        mApp.getDatabase().canUsePool()
            ? make_unique<soci::session>(mApp.getDatabase().getPool())
            : nullptr);
    soci::session& sess(snapSess ? *snapSess : mApp.getDatabase().getSession());
    soci::transaction tx(sess);

    // The current "history block" is stored in _four_ files, one just ledger
    // headers, one TransactionHistoryEntry (which contain txSets),
    // one TransactionHistoryResultEntry containing transaction set results and
    // one (optional) SCPHistoryEntry containing the SCP messages used to close.
    // All files are streamed out of the database, entry-by-entry.
    size_t nbSCPMessages;
    uint32_t begin, count;
    size_t nHeaders;
    {
        XDROutputFileStream ledgerOut, txOut, txResultOut, scpHistory;
        ledgerOut.open(mLedgerSnapFile->localPath_nogz());
        txOut.open(mTransactionSnapFile->localPath_nogz());
        txResultOut.open(mTransactionResultSnapFile->localPath_nogz());
        scpHistory.open(mSCPHistorySnapFile->localPath_nogz());

        // 'mLocalState' describes the LCL, so its currentLedger will usually be
        // 63,
        // 127, 191, etc. We want to start our snapshot at 64-before the _next_
        // ledger: 0, 64, 128, etc. In cases where we're forcibly checkpointed
        // early, we still want to round-down to the previous checkpoint ledger.
        begin = mApp.getHistoryManager().prevCheckpointLedger(
            mLocalState.currentLedger);

        count = (mLocalState.currentLedger - begin) + 1;
        CLOG(DEBUG, "History") << "Streaming " << count
                               << " ledgers worth of history, from " << begin;

        nHeaders = LedgerHeaderFrame::copyLedgerHeadersToStream(
            mApp.getDatabase(), sess, begin, count, ledgerOut);
        size_t nTxs = TransactionFrame::copyTransactionsToStream(
            mApp.getNetworkID(), mApp.getDatabase(), sess, begin, count, txOut,
            txResultOut);
        CLOG(DEBUG, "History") << "Wrote " << nHeaders << " ledger headers to "
                               << mLedgerSnapFile->localPath_nogz();
        CLOG(DEBUG, "History")
            << "Wrote " << nTxs << " transactions to "
            << mTransactionSnapFile->localPath_nogz() << " and "
            << mTransactionResultSnapFile->localPath_nogz();

        nbSCPMessages = HerderPersistence::copySCPHistoryToStream(
            mApp.getDatabase(), sess, begin, count, scpHistory);

        CLOG(DEBUG, "History")
            << "Wrote " << nbSCPMessages << " SCP messages to "
            << mSCPHistorySnapFile->localPath_nogz();
    }

    if (nbSCPMessages == 0)
    {
        // don't upload empty files
        std::remove(mSCPHistorySnapFile->localPath_nogz().c_str());
    }

    // When writing checkpoint 0x3f (63) we will have written 63 headers because
    // header 0 doesn't exist, ledger 1 is the first. For all later checkpoints
    // we will write 64 headers; any less and something went wrong[1].
    //
    // [1]: Probably our read transaction was serialized ahead of the write
    // transaction composing the history itself, despite occurring in the
    // opposite wall-clock order, this is legal behavior in SERIALIZABLE
    // transaction-isolation level -- the highest offered! -- as txns only have
    // to be applied in isolation and in _some_ order, not the wall-clock order
    // we issued them. Anyway this is transient and should go away upon retry.
    if (!((begin == 0 && nHeaders == count - 1) || nHeaders == count))
    {
        CLOG(WARNING, "History")
            << "Only wrote " << nHeaders << " ledger headers for "
            << mLedgerSnapFile->localPath_nogz() << ", expecting " << count
            << ", will retry";
        return false;
    }

    return true;
}
}
