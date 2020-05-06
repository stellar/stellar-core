// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/simulation/TxSimApplyTransactionsWork.h"
#include "herder/LedgerCloseData.h"
#include "herder/simulation/SimulationTxSetFrame.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerRange.h"

namespace stellar
{

TxSimApplyTransactionsWork::TxSimApplyTransactionsWork(
    Application& app, TmpDir const& downloadDir, LedgerRange const& range,
    std::string const& networkPassphrase, uint32_t desiredOperations,
    bool upgrade)
    : BasicWork(app, "apply-transactions", RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mNetworkID(sha256(networkPassphrase))
    , mTransactionHistory{}
    , mTransactionIter(mTransactionHistory.txSet.txs.cend())
    , mResultHistory{}
    , mResultIter(mResultHistory.txResultSet.results.cend())
    , mMaxOperations(desiredOperations)
    , mUpgradeProtocol(upgrade)
{
    auto const& lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (mUpgradeProtocol &&
        lcl.header.ledgerVersion + 1 != Config::CURRENT_LEDGER_PROTOCOL_VERSION)
    {
        throw std::runtime_error("Invalid ledger version: can only force "
                                 "upgrade for consecutive versions");
    }
}

bool
TxSimApplyTransactionsWork::getNextLedgerFromHistoryArchive()
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
TxSimApplyTransactionsWork::getNextLedger(
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

    size_t nOps = 0;
    while (true)
    {
        while (mTransactionIter != mTransactionHistory.txSet.txs.cend() &&
               nOps <= mMaxOperations)
        {
            transactions.emplace_back(*mTransactionIter);
            nOps += mTransactionIter->v0().tx.operations.size();
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
        upgrades.erase(
            std::remove_if(upgrades.begin(), upgrades.end(),
                           [](auto const& opaqueUpgrade) {
                               LedgerUpgrade upgrade;
                               xdr::xdr_from_opaque(opaqueUpgrade, upgrade);
                               return (upgrade.type() ==
                                       LEDGER_UPGRADE_MAX_TX_SET_SIZE);
                           }),
            upgrades.end());
        if (!upgrades.empty())
        {
            return true;
        }
    }
}

void
TxSimApplyTransactionsWork::onReset()
{
    // Upgrade max transaction set size if necessary
    auto& lm = mApp.getLedgerManager();
    auto const& lclHeader = lm.getLastClosedLedgerHeader();
    auto const& header = lclHeader.header;

    // If ledgerVersion < 11 then we need to support at least mMaxOperations
    // transactions to guarantee we can support mMaxOperations operations no
    // matter how they are distributed (worst case one per transaction).
    //
    // If ledgerVersion >= 11 then we need to support at least mMaxOperations
    // operations.
    //
    // So we can do the same upgrade in both cases.
    if (header.maxTxSetSize < mMaxOperations || mUpgradeProtocol)
    {
        StellarValue sv;
        if (header.maxTxSetSize < mMaxOperations)
        {
            LedgerUpgrade upgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
            upgrade.newMaxTxSetSize() = mMaxOperations;
            auto opaqueUpgrade = xdr::xdr_to_opaque(upgrade);
            sv.upgrades.emplace_back(opaqueUpgrade.begin(),
                                     opaqueUpgrade.end());
        }
        if (mUpgradeProtocol)
        {
            LedgerUpgrade upgrade(LEDGER_UPGRADE_VERSION);
            upgrade.newLedgerVersion() =
                Config::CURRENT_LEDGER_PROTOCOL_VERSION;
            auto opaqueUpgrade = xdr::xdr_to_opaque(upgrade);
            sv.upgrades.emplace_back(opaqueUpgrade.begin(),
                                     opaqueUpgrade.end());
        }

        TransactionSet txSetXDR;
        txSetXDR.previousLedgerHash = lclHeader.hash;
        auto txSet = std::make_shared<TxSetFrame>(mNetworkID, txSetXDR);

        sv.txSetHash = txSet->getContentsHash();
        sv.closeTime = header.scpValue.closeTime + 1;

        LedgerCloseData closeData(header.ledgerSeq + 1, txSet, sv);
        lm.closeLedger(closeData);
    }

    // Prepare the HistoryArchiveStream
    mStream = std::make_unique<HistoryArchiveStream>(mDownloadDir, mRange,
                                                     mApp.getHistoryManager());
}

BasicWork::State
TxSimApplyTransactionsWork::onRun()
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
    auto const& header = lclHeader.header;

    auto txSet = std::make_shared<SimulationTxSetFrame>(
        mNetworkID, lclHeader.hash, transactions, results);

    StellarValue sv;
    sv.txSetHash = txSet->getContentsHash();
    sv.closeTime = header.scpValue.closeTime + 1;
    sv.upgrades.insert(sv.upgrades.begin(), upgrades.begin(), upgrades.end());

    LedgerCloseData closeData(header.ledgerSeq + 1, txSet, sv);
    lm.closeLedger(closeData);
    return State::WORK_RUNNING;
}

bool
TxSimApplyTransactionsWork::onAbort()
{
    return true;
}
}
