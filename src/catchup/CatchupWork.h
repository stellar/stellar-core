// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/BucketDownloadWork.h"

namespace stellar
{

class CatchupWork : public BucketDownloadWork
{
  public:
    enum class ProgressState
    {
        APPLIED_BUCKETS,
        APPLIED_TRANSACTIONS,
        FINISHED
    };

    // ProgressHandler is called in different phases of catchup with following
    // values of ProgressState argument:
    // - APPLIED_BUCKETS - called after buckets had been applied at lastClosed
    // ledger
    // - APPLIED_TRANSACTIONS - called after transactions had been applied,
    // last one at lastClosed ledger
    // - FINISHED - called after buckets and transaction had been applied,
    // lastClosed is the same as value from previous call
    //
    // Different types of catchup causes different sequence of calls:
    // - CATCHUP_MINIMAL calls APPLIED_BUCKETS then FINISHED
    // - CATCHUP_COMPLETE calls APPLIED_TRANSACTIONS then FINISHED
    // - CATCHUP_RECENT calls APPLIED_BUCKETS, APPLIED_TRANSACTIONS then
    // FINISHED
    //
    // In case of error this callback is called with non-zero ec parameter and
    // the rest of them does not matter.
    using ProgressHandler = std::function<void(
        asio::error_code const& ec, ProgressState progressState,
        LedgerHeaderHistoryEntry const& lastClosed)>;

  protected:
    HistoryArchiveState mRemoteState;
    uint32_t const mInitLedger;
    bool const mManualCatchup;
    std::shared_ptr<Work> mGetHistoryArchiveStateWork;

    uint32_t nextLedger() const;
    virtual uint32_t archiveStateSeq() const;
    virtual uint32_t firstCheckpointSeq() const = 0;
    virtual uint32_t lastCheckpointSeq() const;

  public:
    CatchupWork(Application& app, WorkParent& parent, uint32_t initLedger,
                std::string const& mode, bool manualCatchup);
    virtual void onReset() override;
};
}
