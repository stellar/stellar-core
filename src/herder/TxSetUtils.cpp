// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetUtils.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/AppConnector.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/BatchExecutor.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/UnorderedSet.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <algorithm>
#include <list>
#include <numeric>

namespace stellar
{
namespace
{
// Target use case is to remove a subset of invalid transactions from a TxSet.
// I.e. txSet.size() >= txsToRemove.size()
TxFrameList
removeTxs(TxFrameList const& txs, TxFrameList const& txsToRemove)
{
    UnorderedSet<Hash> txsToRemoveSet;
    txsToRemoveSet.reserve(txsToRemove.size());
    std::transform(
        txsToRemove.cbegin(), txsToRemove.cend(),
        std::inserter(txsToRemoveSet, txsToRemoveSet.end()),
        [](TransactionFrameBasePtr const& tx) { return tx->getFullHash(); });

    TxFrameList newTxs;
    newTxs.reserve(txs.size() - txsToRemove.size());
    for (auto const& tx : txs)
    {
        if (txsToRemoveSet.find(tx->getFullHash()) == txsToRemoveSet.end())
        {
            newTxs.emplace_back(tx);
        }
    }

    return newTxs;
}
} // namespace

AccountTransactionQueue::AccountTransactionQueue(
    std::vector<TransactionFrameBasePtr> const& accountTxs)
    : mTxs(accountTxs.begin(), accountTxs.end())
{
    releaseAssert(!mTxs.empty());
    std::sort(mTxs.begin(), mTxs.end(),
              [](TransactionFrameBasePtr const& tx1,
                 TransactionFrameBasePtr const& tx2) {
                  return tx1->getSeqNum() < tx2->getSeqNum();
              });
    for (auto const& tx : accountTxs)
    {
        mNumOperations += tx->getNumOperations();
    }
}

TransactionFrameBasePtr
AccountTransactionQueue::getTopTx() const
{
    releaseAssert(!mTxs.empty());
    return mTxs.front();
}

bool
AccountTransactionQueue::empty() const
{
    return mTxs.empty();
}

void
AccountTransactionQueue::popTopTx()
{
    releaseAssert(!mTxs.empty());
    mNumOperations -= mTxs.front()->getNumOperations();
    mTxs.pop_front();
}

bool
TxSetUtils::hashTxSorter(TransactionFrameBasePtr const& tx1,
                         TransactionFrameBasePtr const& tx2)
{
    // need to use the hash of whole tx here since multiple txs could have
    // the same Contents
    return tx1->getFullHash() < tx2->getFullHash();
}

TxFrameList
TxSetUtils::sortTxsInHashOrder(TxFrameList const& transactions)
{
    ZoneScoped;
    TxFrameList sortedTxs(transactions);
    std::sort(sortedTxs.begin(), sortedTxs.end(), TxSetUtils::hashTxSorter);
    return sortedTxs;
}

TxStageFrameList
TxSetUtils::sortParallelTxsInHashOrder(TxStageFrameList const& stages)
{
    ZoneScoped;
    TxStageFrameList sortedStages = stages;
    for (auto& stage : sortedStages)
    {
        for (auto& thread : stage)
        {
            std::sort(thread.begin(), thread.end(), TxSetUtils::hashTxSorter);
        }
        std::sort(stage.begin(), stage.end(), [](auto const& a, auto const& b) {
            releaseAssert(!a.empty() && !b.empty());
            return hashTxSorter(a.front(), b.front());
        });
    }
    std::sort(sortedStages.begin(), sortedStages.end(),
              [](auto const& a, auto const& b) {
                  releaseAssert(!a.empty() && !b.empty());
                  releaseAssert(!a.front().empty() && !b.front().empty());
                  return hashTxSorter(a.front().front(), b.front().front());
              });
    return sortedStages;
}

std::vector<std::shared_ptr<AccountTransactionQueue>>
TxSetUtils::buildAccountTxQueues(TxFrameList const& txs)
{
    ZoneScoped;
    UnorderedMap<AccountID, std::vector<TransactionFrameBasePtr>> actTxMap;

    for (auto const& tx : txs)
    {
        auto id = tx->getSourceID();
        auto it =
            actTxMap.emplace(id, std::vector<TransactionFrameBasePtr>()).first;
        it->second.emplace_back(tx);
    }

    std::vector<std::shared_ptr<AccountTransactionQueue>> queues;
    for (auto const& [_, actTxs] : actTxMap)
    {
        queues.emplace_back(std::make_shared<AccountTransactionQueue>(actTxs));
    }
    return queues;
}

template <typename T>
TxFrameListWithErrors
TxSetUtils::getInvalidTxListWithErrors(
    T const& inTxs, Application& app,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    TxFrameList txs(inTxs.begin(), inTxs.end());

    auto ledgerView = std::make_unique<CheckValidLedgerViewWrapper>(app);
#ifdef BUILD_TESTS
    // See TransactionQueue::canAdd for the overlay-only-mode rationale.
    ledgerView->mSkipSeqNumCheck = app.getRunInOverlayOnlyMode();
#endif
    // Validate minSeqLedgerGap and LedgerBounds against the next ledgerSeq,
    // which is what will be used at apply time.
    std::optional<uint32_t> validationLedgerSeq;
    if (protocolVersionStartsFrom(
            ledgerView->getLedgerHeader().current().ledgerVersion,
            ProtocolVersion::V_19))
    {
        validationLedgerSeq =
            app.getLedgerManager().getLastClosedLedgerNum() + 1;
    }

    // Parallelize transaction validation using the batch executor with
    // `taskCount` batches.
    auto taskCount = app.getBatchExecutor().preferredTaskCount();

    // In some cases we might process a transaction set for ledger N while
    // applying ledger N, in which case we won't be able to re-use the batch
    // executor and thus need to fall back to single-threaded execution.
    // This is really an edge case: it requires several tx sets to exist for the
    // same ledger (which is rare), and the timing must be very specific for
    // processing to happen after the vote and application start. Thus in
    // practice we may consider this rare enough to not worry about the
    // performance impact.
    // NB: Both this method and `isApplying` must happen on the main thread,
    // so there is no race condition risk.
    if (app.getLedgerManager().isApplying())
    {
        taskCount = 1;
    }
#ifdef BUILD_TESTS
    // In in-memory mode we use a raw LTX in validation, which is not safe to
    // share across multiple threads. That's avoidable, but requires changes to
    // LTX and validation logic, so it's not worth for fixing this just for
    // tests.
    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        taskCount = 1;
    }
#endif

    std::vector<std::unique_ptr<CheckValidLedgerViewWrapper>> ledgerViews;
    ledgerViews.emplace_back(std::move(ledgerView));

    for (size_t i = 1; i < taskCount; ++i)
    {
        ledgerViews.emplace_back(
            std::make_unique<CheckValidLedgerViewWrapper>(app));
#ifdef BUILD_TESTS
        // See TransactionQueue::canAdd for the overlay-only-mode
        // rationale.
        ledgerViews.back()->mSkipSeqNumCheck = app.getRunInOverlayOnlyMode();
#endif
    }

    std::vector<std::pair<bool, std::optional<int64_t>>> txValidationResult(
        txs.size());
    auto& appConnector = app.getAppConnector();

    app.getBatchExecutor().executeBatchOverRanges(
        txs.size(), taskCount,
        [&appConnector, &txs, &ledgerViews, &txValidationResult,
         lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
         validationLedgerSeq](size_t begin, size_t end, size_t rangeIndex) {
            auto const& view = *ledgerViews.at(rangeIndex);
            auto const header = view.getLedgerHeader().current();
            auto diagnostics = DiagnosticEventManager::createDisabled();
            for (size_t i = begin; i < end; ++i)
            {
                auto res = txs[i]->checkValid(appConnector, view, 0,
                                              lowerBoundCloseTimeOffset,
                                              upperBoundCloseTimeOffset,
                                              diagnostics, validationLedgerSeq);
                txValidationResult[i].first = res->isSuccess();
                if (!res->isSuccess())
                {
                    continue;
                }
                auto feeSource = view.getAccount(txs[i]->getFeeSourceID());
                if (feeSource)
                {
                    txValidationResult[i].second =
                        getAvailableBalance(header, feeSource.current());
                }
            }
        });

    TxFrameListWithErrors invalidTxsWithError;
    auto& [invalidTxs, errorCode] = invalidTxsWithError;
    errorCode = TxSetValidationResult::VALID;

    for (size_t i = 0; i < txs.size(); ++i)
    {
        auto const& tx = txs[i];
        auto const& [txIsValid, feeSourceBalance] = txValidationResult[i];
        if (!txIsValid)
        {
            invalidTxs.emplace_back(tx);
            errorCode = TxSetValidationResult::TX_VALIDATION_FAILED;
            continue;
        }
        int64_t& accFee = accountFeeMap[tx->getFeeSourceID()];
        if (INT64_MAX - accFee < tx->getFullFee())
        {
            accFee = INT64_MAX;
        }
        else
        {
            accFee += tx->getFullFee();
        }
        // `feeSourceBalance` should exist as transaction must be valid, log
        // an internal error and skip the transaction otherwise.
        if (!feeSourceBalance)
        {
            CLOG_ERROR(Herder,
                       "Account not found when checking TxSet validity");
            CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
            invalidTxs.emplace_back(tx);
            errorCode = TxSetValidationResult::TX_VALIDATION_FAILED;
            continue;
        }
        if (*feeSourceBalance < accFee)
        {
            invalidTxs.push_back(tx);
            // Only override the error code if it wasn't already set.
            if (errorCode == TxSetValidationResult::VALID)
            {
                errorCode = TxSetValidationResult::ACCOUNT_CANT_PAY_FEE;
            }
            CLOG_DEBUG(
                Herder, "Got bad txSet: account can't pay fee tx: {}",
                xdrToCerealString(tx->getEnvelope(), "TransactionEnvelope"));
        }
    }

    return invalidTxsWithError;
}

// Explicit template instantiations for getInvalidTxListWithErrors
template TxFrameListWithErrors
TxSetUtils::getInvalidTxListWithErrors<TxFrameList>(
    TxFrameList const& txs, Application& app,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset);
template TxFrameListWithErrors
TxSetUtils::getInvalidTxListWithErrors<TxSetPhaseFrame>(
    TxSetPhaseFrame const& txs, Application& app,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset);

TxFrameList
TxSetUtils::trimInvalid(TxFrameList const& txs, Application& app,
                        UnorderedMap<AccountID, int64_t>& accountFeeMap,
                        uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset,
                        TxFrameList& invalidTxs)
{
    invalidTxs = getInvalidTxListWithErrors(txs, app, accountFeeMap,
                                            lowerBoundCloseTimeOffset,
                                            upperBoundCloseTimeOffset)
                     .first;
    return removeTxs(txs, invalidTxs);
}

} // namespace stellar
