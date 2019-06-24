// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/simulation/ApplyTransactionsWork.h"
#include "herder/LedgerCloseData.h"
#include "herder/simulation/SimulationTxSetFrame.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerRange.h"

namespace stellar
{

ApplyTransactionsWork::ApplyTransactionsWork(
    Application& app, TmpDir const& downloadDir, LedgerRange const& range,
    std::string const& networkPassphrase, uint32_t desiredOperations)
    : BasicWork(app, "apply-transactions", RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mNetworkID(sha256(networkPassphrase))
    , mTransactionHistory{}
    , mTransactionIter(mTransactionHistory.txSet.txs.cend())
    , mResultHistory{}
    , mResultIter(mResultHistory.txResultSet.results.cend())
    , mMaxOperations(desiredOperations)
{
}

bool
ApplyTransactionsWork::getNextLedgerFromHistoryArchive()
{
    if (mStream->getNextLedger(mHeaderHistory, mTransactionHistory,
                               mResultHistory))
    {
        TxSetFrame txSet(mNetworkID, mTransactionHistory.txSet);
        mTransactionHistory.txSet.txs.clear();
        for (auto const& tx : txSet.sortForApply())
        {
            mTransactionHistory.txSet.txs.emplace_back(tx->getEnvelope());
        }
        mTransactionIter = mTransactionHistory.txSet.txs.cbegin();
        mResultIter = mResultHistory.txResultSet.results.cbegin();
        return true;
    }
    return false;
}

bool
ApplyTransactionsWork::getNextLedger(
    std::vector<TransactionEnvelope>& transactions,
    std::vector<TransactionResultPair>& results,
    std::vector<UpgradeType>& upgrades)
{
    transactions.clear();
    results.clear();
    upgrades.clear();

    if (mTransactionIter == mTransactionHistory.txSet.txs.cend())
    {
        if (!getNextLedgerFromHistoryArchive())
        {
            return false;
        }
    }

    uint32_t nOps = 0;
    while (true)
    {
        while (mTransactionIter != mTransactionHistory.txSet.txs.cend() &&
               nOps <= mMaxOperations)
        {
            transactions.emplace_back(*mTransactionIter);
            nOps += mTransactionIter->tx.operations.size();
            ++mTransactionIter;

            results.emplace_back(*mResultIter);
            ++mResultIter;
        }

        if (mTransactionIter != mTransactionHistory.txSet.txs.cend())
        {
            return true;
        }

        if (!getNextLedgerFromHistoryArchive())
        {
            return true;
        }

        upgrades = mHeaderHistory.header.scpValue.upgrades;
        if (!upgrades.empty())
        {
            return true;
        }
    }
}

void
ApplyTransactionsWork::onReset()
{
    mStream = std::make_unique<HistoryArchiveStream>(mDownloadDir, mRange,
                                                     mApp.getHistoryManager());
}

BasicWork::State
ApplyTransactionsWork::onRun()
{
    std::vector<TransactionEnvelope> transactions;
    std::vector<TransactionResultPair> results;
    std::vector<UpgradeType> upgrades;
    if (!getNextLedger(transactions, results, upgrades))
    {
        return State::WORK_SUCCESS;
    }

    auto& lm = mApp.getLedgerManager();
    auto const& lclHeader = lm.getLastClosedLedgerHeader();

    auto txSet = std::make_shared<SimulationTxSetFrame>(
        mNetworkID, lclHeader.hash, transactions, results);

    StellarValue sv;
    sv.txSetHash = txSet->getContentsHash();
    sv.closeTime = lclHeader.header.scpValue.closeTime + 1;
    sv.upgrades.insert(sv.upgrades.begin(), upgrades.begin(), upgrades.end());

    LedgerCloseData closeData(lclHeader.header.ledgerSeq + 1, txSet, sv);
    lm.closeLedger(closeData);
    return State::WORK_RUNNING;
}

bool
ApplyTransactionsWork::onAbort()
{
    return true;
}
}
