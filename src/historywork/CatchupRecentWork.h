// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/CatchupManager.h"
#include "work/Work.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

// Catchup-recent is just a catchup-minimal to (now - N),
// followed by a catchup-complete to now.
class CatchupRecentWork : public Work
{
  public:
    typedef std::function<void(asio::error_code const& ec,
                               CatchupManager::CatchupMode mode,
                               LedgerHeaderHistoryEntry const& ledger)>
        handler;

  protected:
    std::shared_ptr<Work> mCatchupMinimalWork;
    std::shared_ptr<Work> mCatchupCompleteWork;
    uint32_t mInitLedger;
    bool mManualCatchup;
    handler mEndHandler;
    LedgerHeaderHistoryEntry mFirstVerified;
    LedgerHeaderHistoryEntry mLastApplied;

    handler writeFirstVerified();
    handler writeLastApplied();

  public:
    CatchupRecentWork(Application& app, WorkParent& parent, uint32_t initLedger,
                      bool manualCatchup, handler endHandler);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;
    void onFailureRaise() override;
};
}
