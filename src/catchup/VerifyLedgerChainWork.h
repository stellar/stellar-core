// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"
#include "work/Work.h"
#include <future>
#include <iosfwd>
#include <vector>

namespace stellar
{

class TmpDir;
struct LedgerHeaderHistoryEntry;

// This class verifies ledger chain of a given range by checking the hashes.
// Note that verification is done starting with the latest checkpoint in the
// range, and working its way backwards to the beginning of the range.
class VerifyLedgerChainWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    LedgerRange const mRange;
    uint32_t mCurrCheckpoint;
    LedgerNumHashPair const mLastClosed;
    // The max ledger number and hash that we have verified up to at some time
    // in the past (or genesis if we have no previous verification). Invocations
    // of VerifyLedgerChainWork will verify down to this ledger.
    std::optional<LedgerNumHashPair> const mMaxPrevVerified;

    // Record any instance where the chain we're verifying disagrees with the
    // local node state. This _might_ mean we can't possibly catch up (eg. we're
    // on a too-old core or something) or it _might_ mean we're reading from a
    // corrupt archive. The next flag differentiates cases.
    std::optional<HistoryManager::LedgerVerificationStatus>
        mChainDisagreesWithLocalState;

    // Record whether we were instantiated with an initial head-of-chain hash
    // that we trust to be the network consensus. If this is true, then any
    // local-node disagreement is our problem, not a possible corrupt archive,
    // and we should treat it as an unrecoverable error (i.e. set the promise
    // below, to stop further catchup-retries).
    bool mHasTrustedHash{false};

    // Record if archive validity was verified, and core hit an unrecoverable
    // failure (such as incompatible version)
    std::promise<bool> mFatalFailurePromise;

    // Incoming var to read trusted hash of max ledger from. We use a
    // shared_future here because it allows reading the value multiple
    // times and we might be reset and re-run.
    std::shared_future<LedgerNumHashPair> const mTrustedMaxLedger;

    // Outgoing var to write minimum verified ledger's PreviousLedgerHash to.
    std::promise<LedgerNumHashPair> mVerifiedMinLedgerPrev;

    // Cached read-side of mVerifiedMinLedgerPrev -- unfortunately one can
    // only call get_future once on a promise, so we must build (and retain)
    // a shared_future from the result of that call on construction.
    std::shared_future<LedgerNumHashPair> mVerifiedMinLedgerPrevFuture;

    // Propagation link written on each call to verifyHistoryOfSingleCheckpoint,
    // must match max ledger in current call to verifyHistoryOfSingleCheckpoint.
    LedgerNumHashPair mVerifiedAhead;

    // Max ledger of the min checkpoint in the verified range. This is the
    // "checkpoint ledger" of the min checkpoint during catchup, which is also
    // where the bucket applicator will apply buckets.
    LedgerHeaderHistoryEntry mMaxVerifiedLedgerOfMinCheckpoint{};

    // Buffered ledger hashes that have been verified and optional output stream
    // to write them to.
    std::vector<LedgerNumHashPair> mVerifiedLedgers;
    std::shared_ptr<std::ofstream> mOutputStream;

    HistoryManager::LedgerVerificationStatus verifyHistoryOfSingleCheckpoint();

  public:
    VerifyLedgerChainWork(
        Application& app, TmpDir const& downloadDir, LedgerRange const& range,
        LedgerNumHashPair const& lastClosedLedger,
        std::optional<LedgerNumHashPair> const& maxPrevVerified,
        std::shared_future<LedgerNumHashPair> trustedMaxLedger,
        std::promise<bool>&& fatalFailure,
        std::shared_ptr<std::ofstream> outputStream = nullptr);
    ~VerifyLedgerChainWork() override = default;
    std::string getStatus() const override;

    std::shared_future<LedgerNumHashPair>
    getVerifiedMinLedgerPrev() const
    {
        return mVerifiedMinLedgerPrevFuture;
    }

    LedgerHeaderHistoryEntry
    getMaxVerifiedLedgerOfMinCheckpoint()
    {
        return mMaxVerifiedLedgerOfMinCheckpoint;
    }

  protected:
    void onReset() override;

    BasicWork::State onRun() override;
    void onSuccess() override;
    void onFailureRaise() override;
    bool
    onAbort() override
    {
        return true;
    };
};
}
