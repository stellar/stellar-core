#include "history/CheckpointBuilder.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/XDRStream.h"

namespace stellar
{
void
CheckpointBuilder::maybeOpen(uint32_t ledgerSeq)
{
    ZoneScoped;
    releaseAssert(mApp.getHistoryArchiveManager().publishEnabled());
    if (!mOpen)
    {
        releaseAssert(!mTxResults);
        releaseAssert(!mTxs);
        releaseAssert(!mLedgerHeaders);

        auto checkpoint =
            mApp.getHistoryManager().checkpointContainingLedger(ledgerSeq);
        auto res = FileTransferInfo(FileType::HISTORY_FILE_TYPE_RESULTS,
                                    checkpoint, mApp.getConfig());
        auto txs = FileTransferInfo(FileType::HISTORY_FILE_TYPE_TRANSACTIONS,
                                    checkpoint, mApp.getConfig());
        auto ledger = FileTransferInfo(FileType::HISTORY_FILE_TYPE_LEDGER,
                                       checkpoint, mApp.getConfig());

        // Open files in append mode
        mTxResults = std::make_unique<XDROutputFileStream>(
            mApp.getClock().getIOContext(), /* fsync*/ true);
        mTxResults->open(res.localPath_nogz_dirty());

        mTxs = std::make_unique<XDROutputFileStream>(
            mApp.getClock().getIOContext(), /* fsync*/ true);
        mTxs->open(txs.localPath_nogz_dirty());

        mLedgerHeaders = std::make_unique<XDROutputFileStream>(
            mApp.getClock().getIOContext(), /* fsync*/ true);
        mLedgerHeaders->open(ledger.localPath_nogz_dirty());
        mOpen = true;
    }
}

void
CheckpointBuilder::checkpointComplete(uint32_t checkpoint)
{
    ZoneScoped;
    releaseAssert(mApp.getHistoryArchiveManager().publishEnabled());
    releaseAssert(
        mApp.getHistoryManager().isLastLedgerInCheckpoint(checkpoint));

    // This will close and reset the streams
    mLedgerHeaders.reset();
    mTxs.reset();
    mTxResults.reset();
    mOpen = false;

    auto maybeRename = [&](FileTransferInfo const& ft) {
        if (fs::exists(ft.localPath_nogz()))
        {
            CLOG_INFO(History, "File {} already exists, skipping rename",
                      ft.localPath_nogz());
        }
        else if (!fs::durableRename(
                     ft.localPath_nogz_dirty(), ft.localPath_nogz(),
                     getPublishHistoryDir(ft.getType(), mApp.getConfig())
                         .string()))
        {
            throw std::runtime_error(
                fmt::format("Failed to rename checkpoint file {}",
                            ft.localPath_nogz_dirty()));
        }
    };

    auto res = FileTransferInfo(FileType::HISTORY_FILE_TYPE_RESULTS, checkpoint,
                                mApp.getConfig());
    auto txs = FileTransferInfo(FileType::HISTORY_FILE_TYPE_TRANSACTIONS,
                                checkpoint, mApp.getConfig());
    auto ledger = FileTransferInfo(FileType::HISTORY_FILE_TYPE_LEDGER,
                                   checkpoint, mApp.getConfig());
    maybeRename(res);
    maybeRename(txs);
    maybeRename(ledger);
}

CheckpointBuilder::CheckpointBuilder(Application& app) : mApp(app)
{
}

void
CheckpointBuilder::appendTransactionSet(uint32_t ledgerSeq,
                                        TxSetXDRFrameConstPtr const& txSet,
                                        TransactionResultSet const& resultSet)
{
    ZoneScoped;
    if (!mStartupValidationComplete &&
        ledgerSeq != LedgerManager::GENESIS_LEDGER_SEQ)
    {
        throw std::runtime_error("Startup validation not performed");
    }
    maybeOpen(ledgerSeq);

    if (!resultSet.results.empty())
    {
        TransactionHistoryResultEntry results;
        results.ledgerSeq = ledgerSeq;
        results.txResultSet = resultSet;
        mTxResults->durableWriteOne(results);
    }

    if (txSet->sizeTxTotal() > 0)
    {
        TransactionHistoryEntry txs;
        txs.ledgerSeq = ledgerSeq;

        if (txSet->isGeneralizedTxSet())
        {
            txs.ext.v(1);
            txSet->toXDR(txs.ext.generalizedTxSet());
        }
        else
        {
            txSet->toXDR(txs.txSet);
        }
        mTxs->durableWriteOne(txs);
    }
}

void
CheckpointBuilder::appendLedgerHeader(LedgerHeader const& header)
{
    ZoneScoped;
    if (!mStartupValidationComplete &&
        header.ledgerSeq != LedgerManager::GENESIS_LEDGER_SEQ)
    {
        throw std::runtime_error("Startup validation not performed");
    }
    maybeOpen(header.ledgerSeq);

    LedgerHeaderHistoryEntry lhe;
    lhe.header = header;
    lhe.hash = xdrSha256(header);
    mLedgerHeaders->writeOne(lhe);
    mLedgerHeaders->flush();
}

uint32_t
getLedgerSeq(TransactionHistoryEntry const& entry)
{
    return entry.ledgerSeq;
}

uint32_t
getLedgerSeq(TransactionHistoryResultEntry const& entry)
{
    return entry.ledgerSeq;
}

uint32_t
getLedgerSeq(LedgerHeaderHistoryEntry const& entry)
{
    return entry.header.ledgerSeq;
}

void
CheckpointBuilder::cleanup(uint32_t lcl)
{
    if (mStartupValidationComplete)
    {
        return;
    }

    mTxResults.reset();
    mTxs.reset();
    mLedgerHeaders.reset();
    mOpen = false;

    auto checkpoint = mApp.getHistoryManager().checkpointContainingLedger(lcl);
    auto res = FileTransferInfo(FileType::HISTORY_FILE_TYPE_RESULTS, checkpoint,
                                mApp.getConfig());
    auto txs = FileTransferInfo(FileType::HISTORY_FILE_TYPE_TRANSACTIONS,
                                checkpoint, mApp.getConfig());
    auto ledger = FileTransferInfo(FileType::HISTORY_FILE_TYPE_LEDGER,
                                   checkpoint, mApp.getConfig());

    auto tmpDir =
        mApp.getBucketManager().getTmpDirManager().tmpDir("truncated");

    auto truncate = [&](FileTransferInfo const& ft, auto entry) {
        if (fs::exists(ft.localPath_nogz()))
        {
            CLOG_INFO(History, "File {} already exists, nothing to do",
                      ft.localPath_nogz());
            return;
        }

        // Find a tmp file; any potentially invalid files _must_ be tmp files,
        // because checkpoint files are finalized (renamed) only after ledger is
        // committed.
        auto tmpFile = tmpDir.getName() + "/" + ft.baseName_nogz();
        {
            auto out = XDROutputFileStream(mApp.getClock().getIOContext(),
                                           /* fsync*/ true);
            out.open(tmpFile);
            XDRInputFileStream in;
            in.open(ft.localPath_nogz_dirty());
            while (in)
            {
                try
                {
                    if (!in.readOne(entry))
                    {
                        break;
                    }
                    if (getLedgerSeq(entry) > lcl)
                    {
                        CLOG_INFO(History, "Truncating {} at ledger {}",
                                  ft.localPath_nogz_dirty(), lcl);
                        break;
                    }
                    out.durableWriteOne(entry);
                }
                catch (xdr::xdr_runtime_error const& e)
                {
                    // If we can't read the entry, we likely encountered a
                    // partial write
                    CLOG_INFO(History,
                              "Encountered partial write in {}, truncating",
                              ft.localPath_nogz_dirty());
                    break;
                }
            }
        }

        if (!fs::durableRename(
                tmpFile.c_str(), ft.localPath_nogz_dirty().c_str(),
                getPublishHistoryDir(ft.getType(), mApp.getConfig()).string()))
        {
            throw std::runtime_error("Failed to rename checkpoint file");
        }
    };

    truncate(res, TransactionHistoryResultEntry{});
    truncate(txs, TransactionHistoryEntry{});
    truncate(ledger, LedgerHeaderHistoryEntry{});
    mStartupValidationComplete = true;
}
}