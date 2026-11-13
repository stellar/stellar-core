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
#include "medida/meter.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/BatchExecutor.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
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
    if (txsToRemove.empty())
    {
        return txs;
    }
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
    releaseAssert(threadIsMain());
    return getInvalidTxListWithErrors(
        inTxs, app, TxSetValidationContext::forLCL(app), accountFeeMap,
        lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset);
}

template <typename T>
TxFrameListWithErrors
TxSetUtils::getInvalidTxListWithErrors(
    T const& inTxs, Application& app, TxSetValidationContext const& ctx,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    TxFrameList txs(inTxs.begin(), inTxs.end());

    auto ledgerView = ctx.mMakeView();
    // Validate minSeqLedgerGap and LedgerBounds against the next ledgerSeq,
    // which is what will be used at apply time.
    std::optional<uint32_t> validationLedgerSeq;
    if (protocolVersionStartsFrom(
            ledgerView->getLedgerHeader().current().ledgerVersion,
            ProtocolVersion::V_19))
    {
        validationLedgerSeq = ctx.mLcl.header.ledgerSeq + 1;
    }

    // Parallelize transaction validation using the batch executor with
    // `taskCount` batches. The context bounds the count: the batch executor
    // runs one batch at a time, so a caller that may overlap with another
    // user of it (see `TxSetValidationContext::forLCL`) asks for one task,
    // which runs on the calling thread.
    auto taskCount = std::max<size_t>(
        1,
        std::min(app.getBatchExecutor().preferredTaskCount(), ctx.mMaxTasks));

    std::vector<std::unique_ptr<AbstractLedgerView const>> ledgerViews;
    ledgerViews.emplace_back(std::move(ledgerView));

    for (size_t i = 1; i < taskCount; ++i)
    {
        ledgerViews.emplace_back(ctx.mMakeView());
    }

    std::vector<std::pair<bool, std::optional<int64_t>>> txValidationResult(
        txs.size());
    auto& appConnector = app.getAppConnector();
    // The tasks only read the cache; what they validated themselves is
    // added to it below, by this thread.
    TxValidationCache const* const cache = ctx.mValidationCache;
    std::vector<uint8_t> fromCache(cache ? txs.size() : 0, 0);

    app.getBatchExecutor().executeBatchOverRanges(
        txs.size(), taskCount,
        [&appConnector, &txs, &ledgerViews, &txValidationResult, cache,
         &fromCache, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
         validationLedgerSeq](size_t begin, size_t end, size_t rangeIndex) {
            auto const& view = *ledgerViews.at(rangeIndex);
            auto const header = view.getLedgerHeader().current();
            auto diagnostics = DiagnosticEventManager::createDisabled();
            for (size_t i = begin; i < end; ++i)
            {
                // The main thread needs every candidate's full hash next
                // (removing the invalid ones, sorting the set). Compute and
                // cache it here, in parallel, rather than there. Each frame
                // is in one range only, and the batch completes before the
                // main thread reads it.
                auto const& fullHash = txs[i]->getFullHash();
                if (cache)
                {
                    auto it = cache->find(fullHash);
                    if (it != cache->end())
                    {
                        txValidationResult[i] = {it->second.mValid,
                                                 it->second.mFeeSourceBalance};
                        fromCache[i] = 1;
                        continue;
                    }
                }
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

    if (ctx.mValidationCache)
    {
        size_t hits = 0;
        for (size_t i = 0; i < txs.size(); ++i)
        {
            if (fromCache[i])
            {
                ++hits;
            }
            else if (!txValidityDependsOnCloseTime(txs[i]->getEnvelope()))
            {
                ctx.mValidationCache->emplace(
                    txs[i]->getFullHash(),
                    TxValidationResult{txValidationResult[i].first,
                                       txValidationResult[i].second});
            }
        }
        auto& metrics = app.getMetrics();
        metrics.NewMeter({"herder", "txset", "validation-cache-hit"}, "tx")
            .Mark(hits);
        metrics.NewMeter({"herder", "txset", "validation-cache-miss"}, "tx")
            .Mark(txs.size() - hits);
    }

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
template TxFrameListWithErrors
TxSetUtils::getInvalidTxListWithErrors<TxFrameList>(
    TxFrameList const& txs, Application& app, TxSetValidationContext const& ctx,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset);
template TxFrameListWithErrors
TxSetUtils::getInvalidTxListWithErrors<TxSetPhaseFrame>(
    TxSetPhaseFrame const& txs, Application& app,
    TxSetValidationContext const& ctx,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset);

TxFrameList
TxSetUtils::trimInvalid(TxFrameList const& txs, Application& app,
                        UnorderedMap<AccountID, int64_t>& accountFeeMap,
                        uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset,
                        TxFrameList& invalidTxs,
                        TxValidationCache* validationCache)
{
    releaseAssert(threadIsMain());
    auto ctx = TxSetValidationContext::forLCL(app);
    ctx.mValidationCache = validationCache;
    return trimInvalid(txs, app, ctx, accountFeeMap, lowerBoundCloseTimeOffset,
                       upperBoundCloseTimeOffset, invalidTxs);
}

TxFrameList
TxSetUtils::trimInvalid(TxFrameList const& txs, Application& app,
                        TxSetValidationContext const& ctx,
                        UnorderedMap<AccountID, int64_t>& accountFeeMap,
                        uint64_t lowerBoundCloseTimeOffset,
                        uint64_t upperBoundCloseTimeOffset,
                        TxFrameList& invalidTxs)
{
    invalidTxs = getInvalidTxListWithErrors(txs, app, ctx, accountFeeMap,
                                            lowerBoundCloseTimeOffset,
                                            upperBoundCloseTimeOffset)
                     .first;
    return removeTxs(txs, invalidTxs);
}

TxFrameList
TxSetUtils::trimInvalidPrefix(
    TxFrameList const& txs, size_t targetValidTxs,
    std::function<bool(TransactionFrameBase const&)> const& countsTowardTarget,
    Application& app, UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    TxFrameList& invalidTxs, TxValidationCache* validationCache)
{
    releaseAssert(threadIsMain());
    auto ctx = TxSetValidationContext::forLCL(app);
    ctx.mValidationCache = validationCache;
    return trimInvalidPrefix(txs, targetValidTxs, countsTowardTarget, app, ctx,
                             accountFeeMap, lowerBoundCloseTimeOffset,
                             upperBoundCloseTimeOffset, invalidTxs);
}

TxFrameList
TxSetUtils::trimInvalidPrefix(
    TxFrameList const& txs, size_t targetValidTxs,
    std::function<bool(TransactionFrameBase const&)> const& countsTowardTarget,
    Application& app, TxSetValidationContext const& ctx,
    UnorderedMap<AccountID, int64_t>& accountFeeMap,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    TxFrameList& invalidTxs)
{
    ZoneScoped;
    TxFrameList validTxs;
    invalidTxs.clear();
    size_t counted = 0;
    size_t validated = 0;
    // Each pass validates the candidates right after the previous ones, so
    // fee-source totals accumulate in the same order as when validating the
    // whole prefix at once. The last pass validates everything left, so
    // there are at most three passes.
    for (int pass = 0;
         pass < 3 && counted < targetValidTxs && validated < txs.size(); ++pass)
    {
        size_t const missing = targetValidTxs - counted;
        size_t const left = txs.size() - validated;
        size_t const count = pass == 0   ? std::min(missing, left)
                             : pass == 1 ? std::min(2 * missing, left)
                                         : left;
        TxFrameList const batch(txs.begin() + validated,
                                txs.begin() + validated + count);
        validated += count;
        TxFrameList batchInvalid;
        for (auto const& tx : trimInvalid(
                 batch, app, ctx, accountFeeMap, lowerBoundCloseTimeOffset,
                 upperBoundCloseTimeOffset, batchInvalid))
        {
            if (!countsTowardTarget || countsTowardTarget(*tx))
            {
                ++counted;
            }
            validTxs.push_back(tx);
        }
        invalidTxs.insert(invalidTxs.end(), batchInvalid.begin(),
                          batchInvalid.end());
    }
    return validTxs;
}

// The mempool is fee-ordered and sequence-number-oblivious, so it can hand
// out several transactions from one source account (e.g. a chained pair). A
// tx set may only contain one tx per source account, so keep the lowest
// sequence number per account and let the others wait for a later ledger.
void
TxSetUtils::keepOnePerSourceAccount(TxFrameList& txs)
{
    std::unordered_map<AccountID, size_t> firstBySource;
    TxFrameList kept;
    for (auto const& tx : txs)
    {
        auto [it, inserted] =
            firstBySource.emplace(tx->getSourceID(), kept.size());
        if (inserted)
        {
            kept.push_back(tx);
        }
        else if (tx->getSeqNum() < kept[it->second]->getSeqNum())
        {
            kept[it->second] = tx;
        }
    }
    txs = std::move(kept);
}

// One transaction per source account holds across the phases of a set, not
// only within each: of an account's classic and Soroban candidates, the one
// with the lower sequence number stays (the classic one if they are equal).
void
TxSetUtils::keepOnePerSourceAccount(TxFrameList& classicTxs,
                                    TxFrameList& sorobanTxs)
{
    keepOnePerSourceAccount(classicTxs);
    keepOnePerSourceAccount(sorobanTxs);
    if (classicTxs.empty() || sorobanTxs.empty())
    {
        return;
    }
    std::unordered_map<AccountID, SequenceNumber> classicSeq;
    for (auto const& tx : classicTxs)
    {
        classicSeq.emplace(tx->getSourceID(), tx->getSeqNum());
    }
    std::unordered_set<AccountID> dropFromClassic;
    TxFrameList keptSoroban;
    for (auto const& tx : sorobanTxs)
    {
        auto it = classicSeq.find(tx->getSourceID());
        if (it == classicSeq.end())
        {
            keptSoroban.push_back(tx);
        }
        else if (tx->getSeqNum() < it->second)
        {
            dropFromClassic.insert(it->first);
            keptSoroban.push_back(tx);
        }
    }
    sorobanTxs = std::move(keptSoroban);
    if (!dropFromClassic.empty())
    {
        TxFrameList keptClassic;
        for (auto const& tx : classicTxs)
        {
            if (dropFromClassic.count(tx->getSourceID()) == 0)
            {
                keptClassic.push_back(tx);
            }
        }
        classicTxs = std::move(keptClassic);
    }
}

} // namespace stellar
