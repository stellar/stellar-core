// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/EarlyTxSetSelection.h"
#include "crypto/ByteSlice.h"
#include "crypto/ShortHash.h"
#include "herder/TxSetUtils.h"
#include "main/AppConnector.h"
#include "main/Application.h"
#include "transactions/EventManager.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>

namespace stellar
{
namespace
{
class PredictedLedgerView : public AbstractLedgerView
{
  public:
    PredictedLedgerView(ImmutableLedgerDataPtr previous,
                        std::shared_ptr<LedgerHeader const> header,
                        std::shared_ptr<PredictedAccountChanges const> changes,
                        MetricsRegistry& metrics)
        : mPrevious(std::move(previous), metrics)
        , mHeader(std::move(header))
        , mChanges(std::move(changes))
    {
        releaseAssert(mHeader && mChanges);
    }

    LedgerHeaderWrapper
    getLedgerHeader() const override
    {
        return LedgerHeaderWrapper(mHeader);
    }

    SorobanNetworkConfig const*
    getSorobanNetworkConfig() const override
    {
        return mPrevious.getSorobanNetworkConfig();
    }

    LedgerEntryWrapper
    getAccount(AccountID const& account) const override
    {
        return load(accountKey(account));
    }

    LedgerEntryWrapper
    getAccount(LedgerHeaderWrapper const& header,
               TransactionFrame const& tx) const override
    {
        return getAccount(tx.getSourceID());
    }

    LedgerEntryWrapper
    getAccount(LedgerHeaderWrapper const& header, TransactionFrame const& tx,
               AccountID const& accountID) const override
    {
        return getAccount(accountID);
    }

    LedgerEntryWrapper
    load(LedgerKey const& key) const override
    {
        if (key.type() != ACCOUNT)
        {
            // Nothing is predicted about other entries: the previous
            // ledger's version could be stale.
            throw std::logic_error(
                "the predicted state of a ledger only holds account entries");
        }
        auto entry = mPrevious.loadLiveEntry(key);
        if (!entry)
        {
            return LedgerEntryWrapper(entry);
        }
        auto it = mChanges->find(key.account().accountID);
        if (it == mChanges->end())
        {
            return LedgerEntryWrapper(entry);
        }
        auto predicted = std::make_shared<LedgerEntry>(*entry);
        auto& account = predicted->data.account();
        account.seqNum = std::max(account.seqNum, it->second.mSeqNum);
        account.balance -= std::min(account.balance, it->second.mFees);
        return LedgerEntryWrapper(
            std::shared_ptr<LedgerEntry const>(std::move(predicted)));
    }

  private:
    ApplyLedgerView mPrevious;
    std::shared_ptr<LedgerHeader const> mHeader;
    std::shared_ptr<PredictedAccountChanges const> mChanges;
};
}

void
predictAccountChanges(TransactionFrameBase const& tx,
                      PredictedAccountChanges& changes)
{
    auto& source = changes[tx.getSourceID()];
    source.mSeqNum = std::max(source.mSeqNum, tx.getSeqNum());
    auto& feeSource = changes[tx.getFeeSourceID()];
    auto const fee = std::max<int64_t>(0, tx.getFullFee());
    feeSource.mFees =
        feeSource.mFees > std::numeric_limits<int64_t>::max() - fee
            ? std::numeric_limits<int64_t>::max()
            : feeSource.mFees + fee;
}

std::unique_ptr<AbstractLedgerView const>
makePredictedLedgerView(ImmutableLedgerDataPtr previous,
                        std::shared_ptr<LedgerHeader const> header,
                        std::shared_ptr<PredictedAccountChanges const> changes,
                        MetricsRegistry& metrics)
{
    return std::make_unique<PredictedLedgerView>(
        std::move(previous), std::move(header), std::move(changes), metrics);
}

void
forEachPulledTx(std::vector<uint8_t> const& payload,
                std::function<void(uint8_t const*, size_t)> const& f)
{
    if (payload.size() < 4)
    {
        return;
    }
    uint32_t count = 0;
    std::memcpy(&count, payload.data(), 4);
    size_t offset = 4;
    for (uint32_t i = 0; i < count && offset + 4 <= payload.size(); ++i)
    {
        uint32_t length = 0;
        std::memcpy(&length, payload.data() + offset, 4);
        offset += 4;
        if (length > payload.size() - offset)
        {
            break;
        }
        f(payload.data() + offset, length);
        offset += length;
    }
}

uint64_t
hashPulledTx(uint8_t const* data, size_t length)
{
    // Keyed per process: nobody outside can make one transaction's bytes
    // hash like another's.
    return shortHash::computeHash(ByteSlice(data, length));
}

bool
decodePulledTx(uint8_t const* data, size_t length, TransactionEnvelope& tx)
{
    try
    {
        // XDR is read in 4-byte words from 4-byte aligned memory. Well
        // formed entries are (XDR lengths are multiples of 4), so decode
        // them in place; copy anything else to aligned memory first.
        if ((reinterpret_cast<uintptr_t>(data) & 3) == 0)
        {
            xdr::xdr_get g(data, data + length);
            xdr::xdr_argpack_archive(g, tx);
            g.done();
        }
        else
        {
            std::vector<uint8_t> const copy(data, data + length);
            xdr::xdr_from_opaque(copy, tx);
        }
        return true;
    }
    catch (std::exception const&)
    {
        return false;
    }
}

void
EarlyTxSetJob::prepare(Application& app)
{
    ZoneScoped;
    releaseAssert(mPayload && mClosingTxSet && mPreviousLedger);
    auto const* base = mPayload->data();
    forEachPulledTx(*mPayload, [&](uint8_t const* data, size_t length) {
        mPulled.emplace_back(static_cast<size_t>(data - base), length);
        mSeen.insert(hashPulledTx(data, length));
    });

    // What N does, as far as its transaction set says.
    auto changes = std::make_shared<PredictedAccountChanges>();
    mClosingTxHashes.reserve(mClosingTxSet->sizeTxTotal());
    for (auto const& phase :
         mClosingTxSet->createTransactionFrames(app.getNetworkID()))
    {
        for (auto const& tx : phase)
        {
            mClosingTxHashes.insert(tx->getFullHash());
            predictAccountChanges(*tx, *changes);
        }
    }
    mPredicted = std::move(changes);

    // N's header as far as validation reads it: number, value (close time),
    // and what only an upgrade changes (N has none).
    auto const& previous = mPreviousLedger->getLastClosedLedgerHeader();
    auto header = std::make_shared<LedgerHeader>(previous.header);
    header->ledgerSeq = mClosingLedgerSeq;
    header->previousLedgerHash = previous.hash;
    header->scpValue = mClosingValue;
    mPredictedHeader = std::move(header);
}

std::shared_ptr<EarlyTxSetSelection>
EarlyTxSetJob::select(Application& app)
{
    ZoneScoped;
    auto& metrics = app.getMetrics();
    auto timer =
        metrics.NewTimer({"herder", "txset", "early-job-select"}).TimeScope();

    // Every candidate in mempool order, and what the tasks found.
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStop.load())
        {
            return nullptr;
        }
        size_t total = 0;
        for (auto const& results : mTaskResults)
        {
            total += results.size();
        }
        candidates.reserve(total);
        for (auto& results : mTaskResults)
        {
            candidates.insert(candidates.end(),
                              std::make_move_iterator(results.begin()),
                              std::make_move_iterator(results.end()));
            results.clear();
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& a, Candidate const& b) {
                  return a.mIndex < b.mIndex;
              });
    // The results hold for the predicted state and for the close time this
    // job assumes: they are used by this selection and by nothing else.
    TxValidationCache results;
    results.reserve(candidates.size());
    TxFrameList classicTxs;
    TxFrameList sorobanTxs;
    for (auto const& candidate : candidates)
    {
        results.emplace(candidate.mTx->getFullHash(), candidate.mResult);
        (candidate.mTx->isSoroban() ? sorobanTxs : classicTxs)
            .push_back(candidate.mTx);
    }
    TxSetUtils::keepOnePerSourceAccount(classicTxs, sorobanTxs);

    auto selection = std::make_shared<EarlyTxSetSelection>();
    selection->mLedgerSeq = mClosingLedgerSeq + 1;
    selection->mPreviousLedger = mPreviousLedger;
    selection->mSelectionSeed = mSelectionSeed;
    // The limits of the ledger before N. N carries no upgrades, so they are
    // N's as well; the node compares them with the closed ledger's before it
    // uses the selection.
    selection->mLimits.mLclHeader =
        mPreviousLedger->getLastClosedLedgerHeader().header;
    selection->mLimits.mSorobanConfig =
        mPreviousLedger->hasSorobanConfig()
            ? &mPreviousLedger->getSorobanConfig()
            : nullptr;
    releaseAssert(selection->mLimits.mSorobanConfig);

    PerPhaseTransactionList txPhases;
    txPhases.emplace_back(std::move(classicTxs));
    txPhases.emplace_back(std::move(sorobanTxs));
    PerPhaseTransactionList invalidTxPhases;
    invalidTxPhases.resize(txPhases.size());

    // Validity is read from the tasks' results. The context's view is the
    // predicted state all the same, for anything the tasks did not cover.
    LedgerHeaderHistoryEntry predicted;
    predicted.header = *mPredictedHeader;
    TxSetValidationContext const ctx{
        predicted, selection->mLimits.mSorobanConfig,
        [this, &metrics]() {
            return makePredictedLedgerView(mPreviousLedger, mPredictedHeader,
                                           mPredicted, metrics);
        },
        1, &results};
    auto const offset = ApplyTimeOffset::fromSeconds(mAssumedCloseTimeOffset);
    selection->mPhases = TxSetBuilder::selectPhases(
        txPhases, app, selection->mLimits, ctx, offset, invalidTxPhases, {},
        "early", mSelectionSeed, &selection->mHadTxNotFittingLane);
    if (mStop.load())
    {
        return nullptr;
    }
    // The structure of the selection is validated when it is completed, on
    // the phases as they are then (TxSetBuilder::writeSelection).

    UnorderedSet<TransactionFrameBase const*> selected;
    for (auto const& phase : selection->mPhases)
    {
        for (auto const& tx : phase)
        {
            selected.insert(tx.get());
        }
    }
    selection->mSelected = selected.size();
    selection->mLeftovers.reserve(candidates.size() - selected.size());
    for (auto const& candidate : candidates)
    {
        if (selected.count(candidate.mTx.get()) == 0)
        {
            selection->mLeftovers.push_back(candidate.mTx);
        }
    }
    selection->mSeen = std::move(mSeen);
    selection->mSelectedAt = std::chrono::steady_clock::now();
    return selection;
}

void
EarlyTxSetJob::runTask(Application& app,
                       std::shared_ptr<EarlyTxSetJob> const& job, size_t task)
{
    ZoneScopedN("early tx set selection");
#ifdef BUILD_TESTS
    while (job->mHoldForTesting && job->mHoldForTesting->load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
    {
        std::lock_guard<std::mutex> lock(job->mMutex);
        --job->mPendingTasks;
        if (job->mStop.load())
        {
            if (job->mPendingTasks == 0 && job->mActiveTasks == 0)
            {
                job->mDone.store(true);
            }
            return;
        }
        ++job->mActiveTasks;
    }
    std::vector<Candidate> mine;
    try
    {
        std::call_once(job->mPrepared, [&]() {
            // Must not throw out of call_once.
            try
            {
                job->prepare(app);
            }
            catch (std::exception const& e)
            {
                CLOG_WARNING(Herder,
                             "Early tx set selection could not read its "
                             "input: {}",
                             e.what());
                std::lock_guard<std::mutex> lock(job->mMutex);
                job->mStop.store(true);
            }
        });
        if (!job->mStop.load())
        {
            auto& metrics = app.getMetrics();
            auto view = makePredictedLedgerView(job->mPreviousLedger,
                                                job->mPredictedHeader,
                                                job->mPredicted, metrics);
            auto const& header = *job->mPredictedHeader;
            // The frames keep a reference to the network ID: the
            // application's outlives them.
            Hash const& networkID = app.getNetworkID();
            auto& appConnector = app.getAppConnector();
            auto diagnostics = DiagnosticEventManager::createDisabled();
            bool const supportsSoroban = protocolVersionStartsFrom(
                header.ledgerVersion, SOROBAN_PROTOCOL_VERSION);
            std::optional<uint32_t> validationLedgerSeq;
            if (protocolVersionStartsFrom(header.ledgerVersion,
                                          ProtocolVersion::V_19))
            {
                validationLedgerSeq = job->mClosingLedgerSeq + 1;
            }
            auto const offset = job->mAssumedCloseTimeOffset;
            auto const* base = job->mPayload->data();
            mine.reserve(job->mPulled.size() / job->mTasks + 1);
            for (size_t i = task; i < job->mPulled.size(); i += job->mTasks)
            {
                if (job->mStop.load(std::memory_order_relaxed))
                {
                    break;
                }
                auto const& [at, length] = job->mPulled[i];
                TransactionEnvelope env;
                if (!decodePulledTx(base + at, length, env))
                {
                    continue;
                }
                auto tx = TransactionFrameBase::makeTransactionFromWire(
                    networkID, env);
                if (tx->isSoroban() && !supportsSoroban)
                {
                    continue;
                }
                // N's own transactions are still in the mempool.
                if (job->mClosingTxHashes.count(tx->getFullHash()) != 0)
                {
                    continue;
                }
                TxValidationResult result;
                result.mValid =
                    tx->checkValid(appConnector, *view, 0, offset, offset,
                                   diagnostics, validationLedgerSeq)
                        ->isSuccess();
                if (result.mValid)
                {
                    auto feeSource = view->getAccount(tx->getFeeSourceID());
                    if (feeSource)
                    {
                        result.mFeeSourceBalance =
                            getAvailableBalance(header, feeSource.current());
                    }
                }
                mine.push_back(Candidate{i, std::move(tx), result});
            }
        }
    }
    catch (std::exception const& e)
    {
        // The ledger gets a set built the usual way.
        CLOG_WARNING(Herder, "Early tx set selection failed: {}", e.what());
        mine.clear();
        std::lock_guard<std::mutex> lock(job->mMutex);
        job->mStop.store(true);
    }
    bool last = false;
    {
        std::lock_guard<std::mutex> lock(job->mMutex);
        if (!job->mStop.load())
        {
            job->mTaskResults.at(task) = std::move(mine);
        }
        last = --job->mActiveTasks == 0 && job->mPendingTasks == 0;
    }
    if (!last)
    {
        return;
    }
    // Every task has left: nothing else touches the job's transactions.
    std::shared_ptr<EarlyTxSetSelection> selection;
    if (!job->mStop.load())
    {
        try
        {
            selection = job->select(app);
        }
        catch (std::exception const& e)
        {
            CLOG_WARNING(Herder,
                         "Could not select the TX set for ledger {} ahead of "
                         "time: {}",
                         job->mClosingLedgerSeq + 1, e.what());
        }
    }
    {
        std::lock_guard<std::mutex> lock(job->mMutex);
        if (selection && !job->mStop.load())
        {
            job->mFinishedAfterNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    selection->mSelectedAt - job->mStart)
                    .count();
            job->mSelection = std::move(selection);
        }
    }
    job->mDone.store(true);
}
}
