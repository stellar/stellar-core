#include "history/CheckpointBuilder.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/XDRStream.h"

namespace stellar
{
bool
CheckpointBuilder::ensureOpen(uint32_t ledgerSeq)
{
    ZoneScoped;
    releaseAssert(mApp.getHistoryArchiveManager().publishEnabled());
    if (!mOpen)
    {
        releaseAssert(!mTxResults);
        releaseAssert(!mTxs);
        releaseAssert(!mLedgerHeaders);
        // Don't start writing checkpoint until proper checkpoint boundary
        // This can occur if a node enabled publish mid-checkpoint
        if (mPublishWasDisabled && !HistoryManager::isFirstLedgerInCheckpoint(
                                       ledgerSeq, mApp.getConfig()))
        {
            return false;
        }

        mPublishWasDisabled = false;

        auto checkpoint = HistoryManager::checkpointContainingLedger(
            ledgerSeq, mApp.getConfig());
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
    return true;
}

void
CheckpointBuilder::checkpointComplete(uint32_t checkpoint)
{
    ZoneScoped;
    releaseAssert(mApp.getHistoryArchiveManager().publishEnabled());
    releaseAssert(
        HistoryManager::isLastLedgerInCheckpoint(checkpoint, mApp.getConfig()));

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
        else if (fs::exists(ft.localPath_nogz_dirty()) &&
                 !fs::durableRename(
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
                                        TransactionResultSet const& resultSet,
                                        bool skipStartupCheck)
{
    ZoneScoped;
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
    appendTransactionSet(ledgerSeq, txs, resultSet);
}

void
CheckpointBuilder::appendTransactionSet(uint32_t ledgerSeq,
                                        TransactionHistoryEntry const& txSet,
                                        TransactionResultSet const& resultSet,
                                        bool skipStartupCheck)
{
    ZoneScoped;
    if (!mStartupValidationComplete &&
        ledgerSeq != LedgerManager::GENESIS_LEDGER_SEQ && !skipStartupCheck)
    {
        throw std::runtime_error("Startup validation not performed");
    }

    if (!ensureOpen(ledgerSeq))
    {
        return;
    }

    if (!resultSet.results.empty())
    {
        TransactionHistoryResultEntry results;
        results.ledgerSeq = ledgerSeq;
        results.txResultSet = resultSet;
        mTxResults->durableWriteOne(results);
        mTxs->durableWriteOne(txSet);
    }
}

void
CheckpointBuilder::appendLedgerHeader(LedgerHeader const& header,
                                      bool skipStartupCheck)
{
    ZoneScoped;
    if (!mStartupValidationComplete &&
        header.ledgerSeq != LedgerManager::GENESIS_LEDGER_SEQ &&
        !skipStartupCheck)
    {
        throw std::runtime_error("Startup validation not performed");
    }

    if (!ensureOpen(header.ledgerSeq))
    {
        return;
    }

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
    auto const& cfg = mApp.getConfig();

    auto checkpoint = HistoryManager::checkpointContainingLedger(lcl, cfg);
    auto res =
        FileTransferInfo(FileType::HISTORY_FILE_TYPE_RESULTS, checkpoint, cfg);
    auto txs = FileTransferInfo(FileType::HISTORY_FILE_TYPE_TRANSACTIONS,
                                checkpoint, cfg);
    auto ledger =
        FileTransferInfo(FileType::HISTORY_FILE_TYPE_LEDGER, checkpoint, cfg);

    auto tmpDir =
        mApp.getBucketManager().getTmpDirManager().tmpDir("truncated");

    auto recover = [&](FileTransferInfo const& ft, auto entry,
                       uint32_t enforceLCL) {
        if (fs::exists(ft.localPath_nogz()))
        {
            // Make sure any new checkpoints are deleted
            auto next = FileTransferInfo(
                ft.getType(),
                HistoryManager::checkpointContainingLedger(checkpoint + 1, cfg),
                cfg);
            CLOG_INFO(History, "Deleting next checkpoint files {}",
                      next.localPath_nogz_dirty());
            // This may fail if no files were created before core restarted
            // (which is a valid behavior)
            if (std::remove(next.localPath_nogz_dirty().c_str()) &&
                errno != ENOENT)
            {
                throw std::runtime_error(
                    fmt::format("Failed to delete next checkpoint file {}",
                                next.localPath_nogz_dirty()));
            }
            return;
        }

        if (!fs::exists(ft.localPath_nogz_dirty()))
        {
            CLOG_INFO(History,
                      "Skipping recovery of file {}, does not exist. This can "
                      "occur if publish was previously disabled.",
                      ft.localPath_nogz_dirty());
            mPublishWasDisabled = true;
            return;
        }

        // Find a tmp file; any potentially invalid files _must_ be tmp files,
        // because checkpoint files are finalized (renamed) only after ledger is
        // committed.
        std::filesystem::path dir = tmpDir.getName();
        std::filesystem::path tmpFile = dir / ft.baseName_nogz();
        {
            auto out = XDROutputFileStream(mApp.getClock().getIOContext(),
                                           /* fsync*/ true);
            out.open(tmpFile.string());
            XDRInputFileStream in;
            in.open(ft.localPath_nogz_dirty());
            uint32_t lastReadLedgerSeq = 0;
            while (in)
            {
                try
                {
                    if (!in.readOne(entry))
                    {
                        // If file doesn't end on LCL, it's corrupt
                        if (enforceLCL && lastReadLedgerSeq != lcl)
                        {
                            throw std::runtime_error(
                                fmt::format("Corrupt checkpoint file {}, ends "
                                            "on ledger {}, LCL is {}",
                                            ft.localPath_nogz_dirty(),
                                            getLedgerSeq(entry), lcl));
                        }
                        break;
                    }
                    lastReadLedgerSeq = getLedgerSeq(entry);
                    if (lastReadLedgerSeq > lcl)
                    {
                        CLOG_INFO(History, "Truncating {} at ledger {}",
                                  ft.localPath_nogz_dirty(), lcl);
                        break;
                    }
                    out.writeOne(entry);
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
                tmpFile.string(), ft.localPath_nogz_dirty(),
                getPublishHistoryDir(ft.getType(), mApp.getConfig()).string()))
        {
            throw std::runtime_error("Failed to rename checkpoint file");
        }
    };

    // We can only require ledger header to be at LCL; transactions and results
    // can have gaps (if there were empty ledgers)
    recover(res, TransactionHistoryResultEntry{}, /* enforceLCL */ false);
    recover(txs, TransactionHistoryEntry{}, /* enforceLCL */ false);
    recover(ledger, LedgerHeaderHistoryEntry{}, /* enforceLCL */ true);
    mStartupValidationComplete = true;
}
}