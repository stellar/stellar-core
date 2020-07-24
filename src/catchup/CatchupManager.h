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

class CatchupManager
{
  public:
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
    virtual void startCatchup(CatchupConfiguration configuration,
                              std::shared_ptr<HistoryArchive> archive) = 0;

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

    // popBufferedLedger will throw if there are no buffered ledgers
    virtual bool hasBufferedLedger() const = 0;
    virtual LedgerCloseData const& getFirstBufferedLedger() const = 0;
    virtual LedgerCloseData const& getLastBufferedLedger() const = 0;
    virtual void popBufferedLedger() = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual ~CatchupManager(){};
};
}
