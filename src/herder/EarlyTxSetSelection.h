// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The selection of the next transaction set before the current ledger has
// closed.
//
// Ledger N is going through consensus; the last closed ledger is N-1. A node
// that expects to lead ledger N+1 knows N's transaction set as soon as it
// votes for it, and with it most of what N will do to the candidates of N+1:
// which transactions are used up, which source accounts move their sequence
// number, which fees are charged. An EarlyTxSetJob selects the set for N+1 on
// that prediction, on background threads, while N is still in its ballot.
//
// The prediction is an optimization only. When N has closed, the node
// completes the selection (TxSetBuilder::completeSelection): every one of its
// transactions is validated against N as it really closed, what is not valid
// is removed, and the set is topped up with what the job did not select or
// see. Nothing here decides what is valid; it decides what is worth
// validating later.

#include "herder/TxSetFrame.h"
#include "ledger/ImmutableLedgerView.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-ledger.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace stellar
{
class Application;
class MetricsRegistry;

// What a ledger is predicted to do to one account, read off the ledger's
// transaction set alone.
struct PredictedAccountChange
{
    // The highest sequence number a transaction of the set consumes for the
    // account as its source (a transaction consumes its sequence number
    // whether or not it succeeds), or 0.
    SequenceNumber mSeqNum{0};
    // The fees the set's transactions bid in full with the account as their
    // fee source: an upper bound of what the ledger charges it.
    int64_t mFees{0};
};
using PredictedAccountChanges = UnorderedMap<AccountID, PredictedAccountChange>;

// Adds what `tx`, as a transaction of the ledger being predicted, does to
// its source account and to its fee source.
void predictAccountChanges(TransactionFrameBase const& tx,
                           PredictedAccountChanges& changes);

// A view of the state `previous` with `changes` applied to its accounts, for
// validating transactions on one thread. It answers for accounts only, and
// throws on any other kind of entry rather than answer with the previous
// ledger's version.
std::unique_ptr<AbstractLedgerView const>
makePredictedLedgerView(ImmutableLedgerDataPtr previous,
                        std::shared_ptr<LedgerHeader const> header,
                        std::shared_ptr<PredictedAccountChanges const> changes,
                        MetricsRegistry& metrics);

// Calls `f(data, length)` for every transaction of a GET_TOP_TXS response
// payload ([count:4]([len:4][TransactionEnvelope XDR:len])*), in order. A
// truncated payload ends the list.
void forEachPulledTx(std::vector<uint8_t> const& payload,
                     std::function<void(uint8_t const*, size_t)> const& f);
// A hash of a pulled transaction's bytes, to recognize a transaction that
// was pulled before without decoding it. Equal bytes hash equally within a
// process; a collision makes a new transaction look like a known one.
uint64_t hashPulledTx(uint8_t const* data, size_t length);
// Decodes one pulled transaction into `tx`; false if it does not parse.
bool decodePulledTx(uint8_t const* data, size_t length,
                    TransactionEnvelope& tx);

// What an EarlyTxSetJob produces.
struct EarlyTxSetSelection
{
    // The ledger the set is for (N+1).
    uint32_t mLedgerSeq{0};
    // The selection: both phases, within the limits of N-1.
    std::vector<TxSetPhaseFrame> mPhases;
    // Per phase and surge pricing lane, whether the selection left a
    // transaction out for the lane's limits.
    std::vector<std::vector<bool>> mHadTxNotFittingLane;
    // Every other candidate the job decoded (not in N, not selected), in
    // mempool order: the first candidates of the top-up.
    TxFrameList mLeftovers;
    // hashPulledTx of everything the job pulled, N's transactions included.
    UnorderedSet<uint64_t> mSeen;
    // The limits the selection was made within (those of N-1), and what
    // keeps their Soroban configuration alive.
    TxSetBuildLimits mLimits;
    ImmutableLedgerDataPtr mPreviousLedger;
    // Randomized the order of candidates with equal fee rates.
    size_t mSelectionSeed{0};
    size_t mSelected{0};
    std::chrono::steady_clock::time_point mSelectedAt;
};

struct EarlyTxSetJob
{
    // --- Inputs, set on the main thread before the tasks are posted. ---
    // The ledger in its ballot (N), the value this node votes for and that
    // value's transaction set.
    uint32_t mClosingLedgerSeq{0};
    StellarValue mClosingValue;
    TxSetXDRFrameConstPtr mClosingTxSet;
    // The last closed ledger (N-1).
    ImmutableLedgerDataPtr mPreviousLedger;
    // What the main thread pulled from the mempool.
    std::shared_ptr<std::vector<uint8_t> const> mPayload;
    // The close time the candidates are validated for, as an apply-time
    // offset from N.
    uint64_t mAssumedCloseTimeOffset{0};
    size_t mSelectionSeed{0};
    size_t mTasks{1};
    std::chrono::steady_clock::time_point mStart;

    // Set by whoever gives up on the job (the main thread, or a task that
    // failed); tasks stop at their next transaction and nothing is handed
    // over afterwards.
    std::atomic<bool> mStop{false};

    // --- Written once, by the first task (prepare). ---
    std::once_flag mPrepared;
    // The pulled transactions: where each is in the payload.
    std::vector<std::pair<size_t, size_t>> mPulled;
    UnorderedSet<uint64_t> mSeen;
    UnorderedSet<Hash> mClosingTxHashes;
    std::shared_ptr<PredictedAccountChanges const> mPredicted;
    std::shared_ptr<LedgerHeader const> mPredictedHeader;

    // --- Guarded by mMutex. ---
    std::mutex mMutex;
    size_t mPendingTasks{0};
    size_t mActiveTasks{0};
    struct Candidate
    {
        // Position in the pull (mempool order).
        size_t mIndex{0};
        TransactionFrameBasePtr mTx;
        TxValidationResult mResult;
    };
    // What each task decoded and validated.
    std::vector<std::vector<Candidate>> mTaskResults;
    // The result, once it exists and until it is taken.
    std::shared_ptr<EarlyTxSetSelection> mSelection;

#ifdef BUILD_TESTS
    // While this is set and true the job's tasks wait before they do
    // anything: lets a test have the ledger close, or the next job start,
    // while this one is still running.
    std::shared_ptr<std::atomic<bool>> mHoldForTesting;
#endif

    // Set when the last task has left, with or without a result.
    std::atomic<bool> mDone{false};
    // Nanoseconds from mStart until the selection was handed over, or -1.
    std::atomic<int64_t> mFinishedAfterNs{-1};

    // The body of one of the job's mTasks background tasks.
    static void runTask(Application& app,
                        std::shared_ptr<EarlyTxSetJob> const& job, size_t task);

  private:
    void prepare(Application& app);
    std::shared_ptr<EarlyTxSetSelection> select(Application& app);
};
}
