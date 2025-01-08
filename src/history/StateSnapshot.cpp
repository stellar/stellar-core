// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/StateSnapshot.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "herder/HerderPersistence.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/TransactionSQL.h"
#include "util/GlobalChecks.h"
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
          FileType::HISTORY_FILE_TYPE_LEDGER, mLocalState.currentLedger,
          mApp.getConfig()))

    , mTransactionSnapFile(std::make_shared<FileTransferInfo>(
          FileType::HISTORY_FILE_TYPE_TRANSACTIONS, mLocalState.currentLedger,
          mApp.getConfig()))

    , mTransactionResultSnapFile(std::make_shared<FileTransferInfo>(
          FileType::HISTORY_FILE_TYPE_RESULTS, mLocalState.currentLedger,
          mApp.getConfig()))

    , mSCPHistorySnapFile(std::make_shared<FileTransferInfo>(
          mSnapDir, FileType::HISTORY_FILE_TYPE_SCP, mLocalState.currentLedger))

{
    if (mLocalState.currentBuckets.size() != LiveBucketList::kNumLevels)
    {
        throw std::runtime_error("Invalid HAS: malformed bucketlist");
    }
}

bool
StateSnapshot::writeSCPMessages() const
{
    ZoneScoped;
    std::unique_ptr<soci::session> snapSess(
        mApp.getDatabase().canUsePool()
            ? std::make_unique<soci::session>(mApp.getDatabase().getPool())
            : nullptr);
    soci::session& sess(snapSess ? *snapSess
                                 : mApp.getDatabase().getRawSession());
    soci::transaction tx(sess);

    // The current "history block" is stored in _four_ files, one just ledger
    // headers, one TransactionHistoryEntry (which contain txSets),
    // one TransactionHistoryResultEntry containing transaction set results and
    // one (optional) SCPHistoryEntry containing the SCP messages used to close.
    // Only SCP messages are stream out of database, entry by entry.
    // The rest are built incrementally during ledger close.
    size_t nbSCPMessages;
    uint32_t begin, count;
    {
        bool doFsync = !mApp.getConfig().DISABLE_XDR_FSYNC;
        asio::io_context& ctx = mApp.getClock().getIOContext();

        // Extract SCP messages from the database
        XDROutputFileStream scpHistory(ctx, doFsync);
        scpHistory.open(mSCPHistorySnapFile->localPath_nogz());

        begin = HistoryManager::firstLedgerInCheckpointContaining(
            mLocalState.currentLedger, mApp.getConfig());
        count = HistoryManager::sizeOfCheckpointContaining(
            mLocalState.currentLedger, mApp.getConfig());
        CLOG_DEBUG(History, "Streaming {} ledgers worth of history, from {}",
                   count, begin);

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
        auto b = mApp.getBucketManager().getBucketByHash<LiveBucket>(
            hexToBin256(hash));
        releaseAssert(b);
        addIfExists(std::make_shared<FileTransferInfo>(*b));
    }

    return files;
}
}
