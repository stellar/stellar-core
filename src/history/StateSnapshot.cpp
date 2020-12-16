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
#include "ledger/LedgerHeaderUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/TransactionSQL.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include <Tracy.hpp>

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
    if (mLocalState.currentBuckets.size() != BucketList::kNumLevels)
    {
        throw std::runtime_error("Invalid HAS: malformed bucketlist");
    }
}

bool
StateSnapshot::writeHistoryBlocks() const
{
    ZoneScoped;
    std::unique_ptr<soci::session> snapSess(
        mApp.getDatabase().canUsePool()
            ? std::make_unique<soci::session>(mApp.getDatabase().getPool())
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
        bool doFsync = !mApp.getConfig().DISABLE_XDR_FSYNC;
        asio::io_context& ctx = mApp.getClock().getIOContext();
        XDROutputFileStream ledgerOut(ctx, doFsync), txOut(ctx, doFsync),
            txResultOut(ctx, doFsync), scpHistory(ctx, doFsync);
        ledgerOut.open(mLedgerSnapFile->localPath_nogz());
        txOut.open(mTransactionSnapFile->localPath_nogz());
        txResultOut.open(mTransactionResultSnapFile->localPath_nogz());
        scpHistory.open(mSCPHistorySnapFile->localPath_nogz());

        auto& hm = mApp.getHistoryManager();
        begin = hm.firstLedgerInCheckpointContaining(mLocalState.currentLedger);
        count = hm.sizeOfCheckpointContaining(mLocalState.currentLedger);
        CLOG_DEBUG(History, "Streaming {} ledgers worth of history, from {}",
                   count, begin);

        nHeaders = LedgerHeaderUtils::copyToStream(mApp.getDatabase(), sess,
                                                   begin, count, ledgerOut);
        size_t nTxs =
            copyTransactionsToStream(mApp.getNetworkID(), mApp.getDatabase(),
                                     sess, begin, count, txOut, txResultOut);
        CLOG_DEBUG(History, "Wrote {} ledger headers to {}", nHeaders,
                   mLedgerSnapFile->localPath_nogz());
        CLOG_DEBUG(History, "Wrote {} transactions to {} and {}", nTxs,
                   mTransactionSnapFile->localPath_nogz(),
                   mTransactionResultSnapFile->localPath_nogz());

        nbSCPMessages = HerderPersistence::copySCPHistoryToStream(
            mApp.getDatabase(), sess, begin, count, scpHistory);

        CLOG_DEBUG(History, "Wrote {} SCP messages to {}", nbSCPMessages,
                   mSCPHistorySnapFile->localPath_nogz());
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
    if (nHeaders != count)
    {
        CLOG_WARNING(
            History,
            "Only wrote {} ledger headers for {}, expecting {}, will retry",
            nHeaders, mLedgerSnapFile->localPath_nogz(), count);
        return false;
    }

    return true;
}

std::vector<std::shared_ptr<FileTransferInfo>>
StateSnapshot::differingHASFiles(HistoryArchiveState const& other)
{
    ZoneScoped;
    std::vector<std::shared_ptr<FileTransferInfo>> files{};
    auto addIfExists = [&](std::shared_ptr<FileTransferInfo> const& f) {
        if (f && fs::exists(f->localPath_nogz()))
        {
            files.push_back(f);
        }
    };

    addIfExists(mLedgerSnapFile);
    addIfExists(mTransactionSnapFile);
    addIfExists(mTransactionResultSnapFile);
    addIfExists(mSCPHistorySnapFile);

    for (auto const& hash : mLocalState.differingBuckets(other))
    {
        auto b = mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
        assert(b);
        addIfExists(std::make_shared<FileTransferInfo>(*b));
    }

    return files;
}
}
