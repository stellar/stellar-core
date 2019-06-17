// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/simulation/HistoryArchiveStream.h"
#include "herder/TxSetFrame.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"

namespace stellar
{

HistoryArchiveStream::HistoryArchiveStream(TmpDir const& downloadDir,
                                           LedgerRange const& range,
                                           HistoryManager const& hm)
    : mDownloadDir(downloadDir), mRange(range), mHistoryManager(hm)
{
    if (mRange.mFirst != 1 &&
        mHistoryManager.nextCheckpointLedger(mRange.mFirst) != mRange.mFirst)
    {
        uint32_t prev = mHistoryManager.prevCheckpointLedger(mRange.mFirst);
        readHeaderHistory(std::max<uint32_t>(1, prev));
        readTransactionHistory();
        readResultHistory();
        while (mHeaderHistory.header.ledgerSeq != mRange.mFirst - 1)
        {
            readHeaderHistory();
            readTransactionHistory();
            readResultHistory();
        }
    }
}

LedgerHeaderHistoryEntry
HistoryArchiveStream::readHeaderHistory(uint32_t ledgerSeq)
{
    LedgerHeaderHistoryEntry lhhe;

    if (ledgerSeq == 1 ||
        mHistoryManager.nextCheckpointLedger(ledgerSeq) == ledgerSeq)
    {
        if (mHeaderStream && mHeaderStream.readOne(lhhe))
        {
            throw std::runtime_error(
                "Header stream should be exhausted at end of checkpoint");
        }

        FileTransferInfo info(
            mDownloadDir, HISTORY_FILE_TYPE_LEDGER,
            mHistoryManager.checkpointContainingLedger(ledgerSeq));
        mHeaderStream.close();
        mHeaderStream.open(info.localPath_nogz());
    }

    if (!(mHeaderStream && mHeaderStream.readOne(lhhe)))
    {
        throw std::runtime_error(
            "Header stream should not be exhausted before end of checkpoint");
    }
    assert(lhhe.header.ledgerSeq == ledgerSeq);

    mHeaderHistory = lhhe;
    return lhhe;
}

LedgerHeaderHistoryEntry
HistoryArchiveStream::readHeaderHistory()
{
    return readHeaderHistory(mHeaderHistory.header.ledgerSeq + 1);
}

TransactionHistoryEntry
HistoryArchiveStream::makeEmptyTransactionHistory()
{
    TransactionHistoryEntry txhe;
    txhe.ledgerSeq = mHeaderHistory.header.ledgerSeq + 1;
    txhe.txSet.previousLedgerHash = mHeaderHistory.hash;
    return txhe;
}

TransactionHistoryEntry
HistoryArchiveStream::readTransactionHistory()
{
    TransactionHistoryEntry txhe;

    uint32_t ledgerSeq = mHeaderHistory.header.ledgerSeq;
    if (ledgerSeq == 1 ||
        mHistoryManager.nextCheckpointLedger(ledgerSeq) == ledgerSeq)
    {
        if (mTransactionHistory.ledgerSeq >= ledgerSeq)
        {
            throw std::runtime_error("Buffered transaction history should not "
                                     "be ahead at end of checkpoint");
        }

        if (mTransactionStream && mTransactionStream.readOne(txhe))
        {
            throw std::runtime_error(
                "Transaction stream should be exhausted at end of checkpoint");
        }

        FileTransferInfo info(
            mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS,
            mHistoryManager.checkpointContainingLedger(ledgerSeq));
        mTransactionStream.close();
        mTransactionStream.open(info.localPath_nogz());
    }

    if (mTransactionHistory.ledgerSeq < ledgerSeq && mTransactionStream &&
        mTransactionStream.readOne(txhe))
    {
        // Checkpoint contains more transaction history, buffer next
        mTransactionHistory = txhe;
    }

    if (mTransactionHistory.ledgerSeq == ledgerSeq)
    {
        // Buffer contains transaction set for current header
        if (mTransactionHistory.txSet.previousLedgerHash !=
            mHeaderHistory.header.previousLedgerHash)
        {
            throw std::runtime_error("Incorrect transaction set hash");
        }
        return mTransactionHistory;
    }
    else
    {
        // Buffer ahead or behind
        return makeEmptyTransactionHistory();
    }
}

TransactionHistoryResultEntry
HistoryArchiveStream::makeEmptyResultHistory()
{
    TransactionHistoryResultEntry txre;
    txre.ledgerSeq = mHeaderHistory.header.ledgerSeq + 1;
    return txre;
}

TransactionHistoryResultEntry
HistoryArchiveStream::readResultHistory()
{
    TransactionHistoryResultEntry txre;

    uint32_t ledgerSeq = mHeaderHistory.header.ledgerSeq;
    if (ledgerSeq == 1 ||
        mHistoryManager.nextCheckpointLedger(ledgerSeq) == ledgerSeq)
    {
        if (mResultHistory.ledgerSeq >= ledgerSeq)
        {
            throw std::runtime_error("Buffered result history should not be "
                                     "ahead at end of checkpoint");
        }

        if (mResultStream && mResultStream.readOne(txre))
        {
            throw std::runtime_error(
                "Result stream should be exhausted at end of checkpoint");
        }

        FileTransferInfo info(
            mDownloadDir, HISTORY_FILE_TYPE_RESULTS,
            mHistoryManager.checkpointContainingLedger(ledgerSeq));
        mResultStream.close();
        mResultStream.open(info.localPath_nogz());
    }

    if (mResultHistory.ledgerSeq < ledgerSeq && mResultStream &&
        mResultStream.readOne(txre))
    {
        // Checkpoint contains more result history, buffer next
        mResultHistory = txre;
    }

    if (mResultHistory.ledgerSeq == ledgerSeq)
    {
        // Buffer contains transaction set for current header
        return mResultHistory;
    }
    else
    {
        // Buffer ahead or behind
        return makeEmptyResultHistory();
    }
}

bool
HistoryArchiveStream::getNextLedger(LedgerHeaderHistoryEntry& header,
                                    TransactionHistoryEntry& transaction,
                                    TransactionHistoryResultEntry& result)
{
    if (mHeaderHistory.header.ledgerSeq < mRange.mLast)
    {
        if (mHeaderHistory.header.ledgerSeq == 0)
        {
            header = readHeaderHistory(mRange.mFirst);
        }
        else
        {
            header = readHeaderHistory();
        }
        transaction = readTransactionHistory();
        result = readResultHistory();
        return true;
    }
    return false;
}
}
