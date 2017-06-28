#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
struct HistoryArchiveState;
struct LedgerHeaderHistoryEntry;

class CatchupManager
{
  public:
    // The two supported styles of catchup. CATCHUP_COMPLETE will replay all
    // history blocks, from the last closed ledger to the present, when catching
    // up; CATCHUP_MINIMAL will attempt to "fast forward" to the next BucketList
    // checkpoint, skipping the history log that happened between the last
    // closed ledger and the catchup point. This is set by config, default is
    // CATCHUP_MINIMAL but it should be CATCHUP_COMPLETE for any server with
    // API clients. See LedgerManager::startCatchUp and its callers for uses.
    //
    // CATCHUP_RECENT is a hybrid mode that does a CATCHUP_MINIMAL to a point
    // in the recent past, then runs CATCHUP_COMPLETE from there forward to
    // the present.
    //
    // CATCHUP_COMPLETE_IMMEDIATE is a version of CATCHUP_COMPLETE that does not
    // wait for next ledger checkpoint - it uses last checkpoint available in
    // history. It is not usable in normal mode of stellar-core, but can be used
    // to download and apply history without joining to SCP. It can be called by
    // executing `stellar-core --catchup-complete`.
    enum CatchupMode
    {
        CATCHUP_COMPLETE,
        CATCHUP_COMPLETE_IMMEDIATE,
        CATCHUP_MINIMAL,
        CATCHUP_RECENT
    };

    static std::unique_ptr<CatchupManager> create(Application& app);

    // Callback from catchup, indicates that any catchup work is done.
    virtual void historyCaughtup() = 0;

    // Run catchup, we've just heard `initLedger` from the network. Mode can be
    // CATCHUP_COMPLETE, meaning replay history from last to present, or
    // CATCHUP_MINIMAL, meaning snap to the next state possible and discard
    // history. See larger comment above for more detail.
    //
    // The `manualCatchup` flag modifies catchup behavior to avoid rounding up
    // to the next scheduled checkpoint boundary, instead catching up to a
    // checkpoint presumed to have been made at `initLedger` (i.e. with
    // checkpoint ledger number equal to initLedger-1). This 'manual' catchup
    // mode exists to support catching-up to manually created checkpoints.
    virtual void catchupHistory(
        uint32_t initLedger, CatchupMode mode,
        std::function<void(asio::error_code const& ec, CatchupMode mode,
                           LedgerHeaderHistoryEntry const& lastClosed)>
            handler,
        bool manualCatchup = false) = 0;

    // Return status of catchup for or empty string, if no catchup in progress
    virtual std::string getStatus() const = 0;

    // Return the number of times the process has commenced catchup.
    virtual uint64_t getCatchupStartCount() const = 0;

    // Return the number of times the catchup has completed successfully.
    virtual uint64_t getCatchupSuccessCount() const = 0;

    // Return the number of times the catchup has failed.
    virtual uint64_t getCatchupFailureCount() const = 0;

    virtual ~CatchupManager(){};
};
}
