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
#include "util/UnorderedSet.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <algorithm>
#include <exception>
#include <list>
#include <numeric>
#include <thread>

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

void
addFeeWithSaturation(UnorderedMap<AccountID, int64_t>& accountFeeMap,
                     AccountID const& feeSourceID, int64_t fee)
{
    int64_t& accFee = accountFeeMap[feeSourceID];
    if (INT64_MAX - accFee < fee)
    {
        accFee = INT64_MAX;
    }
    else
    {
        accFee += fee;
    }
}

void
mergeAccountFeeMaps(UnorderedMap<AccountID, int64_t>& destination,
                    UnorderedMap<AccountID, int64_t> const& source)
{
    for (auto const& [feeSourceID, fee] : source)
    {
        addFeeWithSaturation(destination, feeSourceID, fee);
    }
}

size_t
getValidationThreadCount(size_t txCount)
{
    if (txCount == 0)
    {
        return 0;
    }

    auto const hardwareThreads = std::thread::hardware_concurrency();
    auto const targetThreadCount =
        hardwareThreads > 1 ? static_cast<size_t>(hardwareThreads - 1) : 1;
    return std::min(txCount, targetThreadCount);
}

struct ValidationChunkResult
{
    TxFrameList mInvalidTxs;
    UnorderedMap<AccountID, int64_t> mAccountFeeMap;
    bool mHadValidationFailure = false;
};

void
validateTxChunk(TxFrameList const& txList, size_t chunkBegin, size_t chunkEnd,
                AppConnector& appConnector,
                LedgerStateSnapshot const& ledgerStateSnapshot,
                uint32_t nextLedgerSeq,
                uint64_t lowerBoundCloseTimeOffset,
                uint64_t upperBoundCloseTimeOffset,
                SorobanNetworkConfig const* sorobanConfig,
                ValidationChunkResult& chunkResult)
{
    auto diagnostics = DiagnosticEventManager::createDisabled();
    chunkResult.mInvalidTxs.reserve(chunkEnd - chunkBegin);
    chunkResult.mAccountFeeMap.reserve(chunkEnd - chunkBegin);

    LedgerSnapshot chunkSnapshot(ledgerStateSnapshot);
    chunkSnapshot.getLedgerHeader().currentToModify().ledgerSeq =
        nextLedgerSeq;

    for (size_t txIndex = chunkBegin; txIndex < chunkEnd; ++txIndex)
    {
        auto const& tx = txList[txIndex];
        auto txResult = tx->checkValid(
            appConnector, chunkSnapshot, 0, lowerBoundCloseTimeOffset,
            upperBoundCloseTimeOffset, diagnostics, sorobanConfig);
        if (!txResult->isSuccess())
        {
            chunkResult.mInvalidTxs.emplace_back(tx);
            chunkResult.mHadValidationFailure = true;
        }
        else
        {
            addFeeWithSaturation(chunkResult.mAccountFeeMap,
                                 tx->getFeeSourceID(), tx->getFullFee());
        }
    }
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
    auto txList = TxFrameList(txs.begin(), txs.end());
    auto const nextLedgerSeq =
        app.getLedgerManager().getLastClosedLedgerNum() + 1;

    TxFrameListWithErrors invalidTxsWithError;
    auto& invalidTxs = invalidTxsWithError.first;
    auto& errorCode = invalidTxsWithError.second;
    errorCode = TxSetValidationResult::VALID;

    std::unordered_set<Hash> seenInvalidTxs;

    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        LedgerSnapshot ls(app);
        ls.getLedgerHeader().currentToModify().ledgerSeq = nextLedgerSeq;
        auto const* sorobanConfig = protocolVersionStartsFrom(
                                        ls.getLedgerHeader()
                                            .current()
                                            .ledgerVersion,
                                        SOROBAN_PROTOCOL_VERSION)
                                        ? &app.getLedgerManager()
                                               .getLastClosedSorobanNetworkConfig()
                                        : nullptr;
        auto diagnostics = DiagnosticEventManager::createDisabled();
        for (auto const& tx : txList)
        {
            auto txResult = tx->checkValid(
                app.getAppConnector(), ls, 0, lowerBoundCloseTimeOffset,
                upperBoundCloseTimeOffset, diagnostics, sorobanConfig);
            if (!txResult->isSuccess())
            {
                invalidTxs.emplace_back(tx);
                seenInvalidTxs.emplace(tx->getFullHash());
                errorCode = TxSetValidationResult::TX_VALIDATION_FAILED;
            }
            else
            {
                addFeeWithSaturation(accountFeeMap, tx->getFeeSourceID(),
                                     tx->getFullFee());
            }
        }
    }
    else
    {
        auto const ledgerStateSnapshot =
            app.getLedgerManager().copyLedgerStateSnapshot();
        LedgerSnapshot ls(ledgerStateSnapshot);
        // This is done so minSeqLedgerGap is validated against the next
        // ledgerSeq, which is what will be used at apply time
        ls.getLedgerHeader().currentToModify().ledgerSeq = nextLedgerSeq;
        auto const* sorobanConfig = protocolVersionStartsFrom(
                                        ls.getLedgerHeader()
                                            .current()
                                            .ledgerVersion,
                                        SOROBAN_PROTOCOL_VERSION)
                                        ? &app.getLedgerManager()
                                               .getLastClosedSorobanNetworkConfig()
                                        : nullptr;

        auto const numThreads = getValidationThreadCount(txList.size());
        if (numThreads != 0)
        {
            std::vector<ValidationChunkResult> validationResults(numThreads);
            auto const baseChunkSize = txList.size() / numThreads;
            auto const extraTxs = txList.size() % numThreads;
            if (numThreads == 1)
            {
                validateTxChunk(txList, 0, txList.size(),
                                app.getAppConnector(), ledgerStateSnapshot,
                                nextLedgerSeq, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset, sorobanConfig,
                                validationResults[0]);
            }
            else
            {
                std::vector<std::exception_ptr> validationExceptions(numThreads);
                std::vector<std::thread> threads;
                threads.reserve(numThreads);

                size_t chunkBegin = 0;
                for (size_t threadIndex = 0; threadIndex < numThreads;
                     ++threadIndex)
                {
                    auto const chunkSize =
                        baseChunkSize + (threadIndex < extraTxs ? 1u : 0u);
                    auto const chunkEnd = chunkBegin + chunkSize;
                    threads.emplace_back(
                        [&, threadIndex, chunkBegin, chunkEnd]() {
                            try
                            {
                                validateTxChunk(
                                    txList, chunkBegin, chunkEnd,
                                    app.getAppConnector(), ledgerStateSnapshot,
                                    nextLedgerSeq, lowerBoundCloseTimeOffset,
                                    upperBoundCloseTimeOffset, sorobanConfig,
                                    validationResults[threadIndex]);
                            }
                            catch (...)
                            {
                                validationExceptions[threadIndex] =
                                    std::current_exception();
                            }
                        });

                    chunkBegin = chunkEnd;
                }

                for (auto& thread : threads)
                {
                    thread.join();
                }

                for (auto const& validationException : validationExceptions)
                {
                    if (validationException)
                    {
                        std::rethrow_exception(validationException);
                    }
                }
            }

            for (auto& validationResult : validationResults)
            {
                if (validationResult.mHadValidationFailure)
                {
                    errorCode = TxSetValidationResult::TX_VALIDATION_FAILED;
                }

                for (auto const& invalidTx : validationResult.mInvalidTxs)
                {
                    invalidTxs.emplace_back(invalidTx);
                    seenInvalidTxs.emplace(invalidTx->getFullHash());
                }

                mergeAccountFeeMaps(accountFeeMap,
                                    validationResult.mAccountFeeMap);
            }
        }
    }

    auto validateFeeBalances = [&](LedgerSnapshot& ls) {
        auto header = ls.getLedgerHeader().current();
        for (auto const& tx : txList)
        {
            // Already added invalid tx
            if (seenInvalidTxs.find(tx->getFullHash()) != seenInvalidTxs.end())
            {
                continue;
            }

            auto feeSourceID = tx->getFeeSourceID();
            auto feeSource = ls.getAccount(feeSourceID);
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
                    xdrToCerealString(tx->getEnvelope(),
                                      "TransactionEnvelope"));
            }
        }
    };

    if (app.getConfig().MODE_USES_IN_MEMORY_LEDGER)
    {
        LedgerSnapshot ls(app);
        ls.getLedgerHeader().currentToModify().ledgerSeq = nextLedgerSeq;
        validateFeeBalances(ls);
    }
    else
    {
        auto const ledgerStateSnapshot =
            app.getLedgerManager().copyLedgerStateSnapshot();
        LedgerSnapshot ls(ledgerStateSnapshot);
        ls.getLedgerHeader().currentToModify().ledgerSeq = nextLedgerSeq;
        validateFeeBalances(ls);
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
