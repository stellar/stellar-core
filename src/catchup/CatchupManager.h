#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "herder/LedgerCloseData.h"
#include <functional>
#include <memory>
#include <system_error>

namespace asio
{

typedef std::error_code error_code;
};

namespace stellar
{

class Application;
class CatchupMetrics;
class FileTransferInfo;

class CatchupManager
{
  public:
    struct CatchupMetrics
    {
        uint64_t mHistoryArchiveStatesDownloaded;
        uint64_t mCheckpointsDownloaded;
        uint64_t mLedgersVerified;
        uint64_t mLedgerChainsVerificationFailed;
        uint64_t mBucketsDownloaded;
        uint64_t mBucketsApplied;
        uint64_t mTxSetsDownloaded;
        uint64_t mTxSetsApplied;

        CatchupMetrics();

        CatchupMetrics(uint64_t historyArchiveStatesDownloaded,
                       uint64_t checkpointsDownloaded, uint64_t ledgersVerified,
                       uint64_t ledgerChainsVerificationFailed,
                       uint64_t bucketsDownloaded, uint64_t bucketsApplied,
                       uint64_t txSetsDownloaded, uint64_t txSetsApplied);

        friend CatchupMetrics operator-(CatchupMetrics const& x,
                                        CatchupMetrics const& y);
    };
    static std::unique_ptr<CatchupManager> create(Application& app);

    // Process ledgers that could not be applied, and determine if catchup
    // should run
    virtual void processLedger(LedgerCloseData const& ledgerData) = 0;

    // Forcibly switch the application into catchup mode, treating `toLedger`
    // as the destination ledger number and count as the number of past ledgers
    // that should be replayed. Normally this happens automatically when
    // LedgerManager detects it is desynchronized from SCP's consensus ledger.
    // This method is present in the public interface to permit testing and
    // offline catchups.
    virtual void
    startCatchup(CatchupConfiguration configuration,
                 std::shared_ptr<HistoryArchive> archive,
                 std::set<std::shared_ptr<Bucket>> bucketsToRetain) = 0;

    // Return status of catchup for or empty string, if no catchup in progress
    virtual std::string getStatus() const = 0;

    // Return state of the CatchupWork object
    virtual BasicWork::State getCatchupWorkState() const = 0;
    virtual bool catchupWorkIsDone() const = 0;
    virtual bool isCatchupInitialized() const = 0;

    // Emit a log message and set StatusManager HISTORY_CATCHUP status to
    // describe current catchup state. The `contiguous` argument is passed in
    // to describe whether the ledger-manager's view of current catchup tasks
    // is currently contiguous or discontiguous. Message is passed as second
    // argument.
    virtual void logAndUpdateCatchupStatus(bool contiguous,
                                           std::string const& message) = 0;

    // Emit a log message and set StatusManager HISTORY_CATCHUP status to
    // describe current catchup state. The `contiguous` argument is passed in
    // to describe whether the ledger-manager's view of current catchup tasks
    // is currently contiguous or discontiguous. Message is taken from current
    // work item.
    virtual void logAndUpdateCatchupStatus(bool contiguous) = 0;

    // This returns the ledger that comes immediately after the LCL (i.e., LCL +
    // 1) if CatchupManager has it in its buffer. If not, it doesn’t return any
    // ledger. This method doesn’t tell if the buffer is empty or if it's ever
    // heard of LCL + 1. It only tells if CatchupManager has LCL + 1 in its
    // buffer right now.
    virtual std::optional<LedgerCloseData>
    maybeGetNextBufferedLedgerToApply() = 0;

    // This returns the largest ledger sequence that CatchupManager has ever
    // heard of.
    virtual uint32_t getLargestLedgerSeqHeard() const = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual CatchupMetrics const& getCatchupMetrics() = 0;

    virtual ~CatchupManager(){};

    virtual void historyArchiveStatesDownloaded(uint32_t num = 1) = 0;
    virtual void ledgersVerified(uint32_t num = 1) = 0;
    virtual void ledgerChainsVerificationFailed(uint32_t num = 1) = 0;
    virtual void bucketsApplied(uint32_t num = 1) = 0;
    virtual void txSetsApplied(uint32_t num = 1) = 0;
    virtual void fileDownloaded(std::string type, uint32_t num = 1) = 0;
};
}
