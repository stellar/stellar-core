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
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/ThreadPool.h"
#include "util/UnorderedSet.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <algorithm>
#include <future>
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
    T const& txs, Application& app,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    // Materialize the input into a flat list so that it can be chunked across
    // worker threads.
    std::vector<TransactionFrameBasePtr> txList(txs.begin(), txs.end());

    CheckValidLedgerViewWrapper ledgerView(app);
#ifdef BUILD_TESTS
    // See TransactionQueue::canAdd for the overlay-only-mode rationale.
    bool const skipSeqNumCheck = app.getRunInOverlayOnlyMode();
    ledgerView.mSkipSeqNumCheck = skipSeqNumCheck;
#endif
    // Validate minSeqLedgerGap and LedgerBounds against the next ledgerSeq,
    // which is what will be used at apply time.
    std::optional<uint32_t> validationLedgerSeq;
    if (protocolVersionStartsFrom(
            ledgerView.getLedgerHeader().current().ledgerVersion,
            ProtocolVersion::V_19))
    {
        validationLedgerSeq =
            app.getLedgerManager().getLastClosedLedgerNum() + 1;
    }

    // Run the per-transaction checkValid calls in parallel: every check is an
    // independent read-only query against the immutable LCL snapshot.
    // Results are merged on the main thread below, in input order, so the
    // outcome is identical to the sequential loop.
    std::vector<uint8_t> txIsValid(txList.size(), 0);
    auto& appConnector = app.getAppConnector();
    auto checkRange = [&txList, &txIsValid, &appConnector,
                       lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
                       validationLedgerSeq](
                          size_t begin, size_t end,
                          CheckValidLedgerViewWrapper const& view) {
        auto diagnostics = DiagnosticEventManager::createDisabled();
        for (size_t i = begin; i < end; ++i)
        {
            auto txResult = txList[i]->checkValid(
                appConnector, view, 0, lowerBoundCloseTimeOffset,
                upperBoundCloseTimeOffset, diagnostics, validationLedgerSeq);
            txIsValid[i] = txResult->isSuccess();
        }
    };

    // Don't spawn a thread for less than this many transactions.
    size_t const MIN_TXS_PER_THREAD = 50;
    size_t nThreads =
        std::min<size_t>(std::max(1, app.getConfig().LEDGER_CLOSE_WORKER_THREADS),
                         txList.size() / MIN_TXS_PER_THREAD);
#ifdef BUILD_TESTS
    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        // The in-memory-ledger test mode is backed by a read-only LedgerTxn
        // instead of a bucket-list snapshot and is not safe to query from
        // multiple threads.
        nThreads = 1;
    }
#endif
    if (nThreads <= 1)
    {
        checkRange(0, txList.size(), ledgerView);
    }
    else
    {
        // Each worker validates against its own view wrapper: the underlying
        // immutable snapshot data is shared, the per-view bucket stream
        // caches are not thread-safe. The wrappers must be created on the
        // main thread.
        std::vector<std::unique_ptr<CheckValidLedgerViewWrapper>> workerViews;
        workerViews.reserve(nThreads - 1);
        for (size_t t = 0; t + 1 < nThreads; ++t)
        {
            workerViews.emplace_back(
                std::make_unique<CheckValidLedgerViewWrapper>(app));
#ifdef BUILD_TESTS
            workerViews.back()->mSkipSeqNumCheck = skipSeqNumCheck;
#endif
        }
        // Run on the persistent apply thread pool (idle during tx set
        // building) to keep the workers' allocator caches warm.
        auto& threadPool = app.getApplyThreadPool();
        threadPool.ensureWorkerCount(nThreads - 1);
        size_t const chunkSize = (txList.size() + nThreads - 1) / nThreads;
        std::vector<std::future<void>> futures;
        futures.reserve(nThreads - 1);
        for (size_t t = 0; t + 1 < nThreads; ++t)
        {
            size_t const begin = t * chunkSize;
            size_t const end = std::min(begin + chunkSize, txList.size());
            futures.emplace_back(
                threadPool.submit([&checkRange, &workerViews, t, begin, end]() {
                    checkRange(begin, end, *workerViews[t]);
                }));
        }
        // The main thread processes the last chunk.
        checkRange((nThreads - 1) * chunkSize, txList.size(), ledgerView);
        for (auto& future : futures)
        {
            future.get();
        }
    }

    TxFrameListWithErrors invalidTxsWithError;
    auto& invalidTxs = invalidTxsWithError.first;
    auto& errorCode = invalidTxsWithError.second;
    errorCode = TxSetValidationResult::VALID;

    std::unordered_set<Hash> seenInvalidTxs;
    for (size_t i = 0; i < txList.size(); ++i)
    {
        auto const& tx = txList[i];
        if (!txIsValid[i])
        {
            invalidTxs.emplace_back(tx);
            seenInvalidTxs.emplace(tx->getFullHash());
            errorCode = TxSetValidationResult::TX_VALIDATION_FAILED;
        }
        else
        {
            int64_t& accFee = accountFeeMap[tx->getFeeSourceID()];
            if (INT64_MAX - accFee < tx->getFullFee())
            {
                accFee = INT64_MAX;
            }
            else
            {
                accFee += tx->getFullFee();
            }
        }
    }

    auto header = ledgerView.getLedgerHeader().current();
    for (auto const& tx : txList)
    {
        // Already added invalid tx
        if (seenInvalidTxs.find(tx->getFullHash()) != seenInvalidTxs.end())
        {
            continue;
        }

        auto feeSourceID = tx->getFeeSourceID();
        auto feeSource = ledgerView.getAccount(feeSourceID);
        // feeSource should exist since we've already run checkValid, log
        // internal bug
        if (!feeSource)
        {
            CLOG_ERROR(Herder,
                       "Account not found when checking TxSet validity");
            CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
            continue;
        }
        auto it = accountFeeMap.find(feeSourceID);
        auto totFee = it->second;
        if (getAvailableBalance(header, feeSource.current()) < totFee)
        {
            invalidTxs.push_back(tx);
            // Only override the error code if it wasn't already set
            if (errorCode == TxSetValidationResult::VALID)
            {
                errorCode = TxSetValidationResult::ACCOUNT_CANT_PAY_FEE;
            }
            releaseAssert(seenInvalidTxs.insert(tx->getFullHash()).second);
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
