// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "work/BasicWork.h"
#include "xdr/Stellar-types.h"

namespace stellar
{
/*
 * Verify transaction results for a checkpoint. This work requires
 * downloaded ledger header and transaction result files.
 * */
class VerifyTxResultsWork : public BasicWork
{
    TmpDir const& mDownloadDir;
    uint32_t const mCheckpoint;
    TransactionHistoryResultEntry mTxResultEntry;
    XDRInputFileStream mHdrIn;
    XDRInputFileStream mResIn;
    bool mDone{false};
    asio::error_code mEc;
    uint32_t mLastSeenLedger;

    TransactionHistoryResultEntry getCurrentTxResultSet(uint32_t ledger);
    bool verifyTxResultsOfCheckpoint();

  public:
    VerifyTxResultsWork(Application& app, TmpDir const& downloadDir,
                        uint32_t checkpoint);

  protected:
    BasicWork::State onRun() override;
    void onReset() override;

    bool
    onAbort() override
    {
        return true;
    };
};
}
