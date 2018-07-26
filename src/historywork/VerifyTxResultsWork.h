// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/BatchableWork.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "work/Work.h"
#include "xdr/Stellar-types.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class Bucket;

class VerifyTxResultsWork : public Work
{
    // Transaction results verification work for one checkpoint:
    // Ensures that every TransactionResultSet hashes
    // to correct value in ledger header.
    // The assumption here is that ledger chain has already
    // been verified and we trust its data.

    TmpDir const& mDownloadDir;
    uint32_t mCheckpoint;

    // Verify metrics
    medida::Meter& mVerifyTxResultSetStart;
    medida::Meter& mVerifyTxResultSetSuccess;
    medida::Meter& mVerifyTxResultSetFailure;

    XDRInputFileStream mHdrIn;
    XDRInputFileStream mResIn;
    uint32_t mCurrLedger;
    TransactionHistoryResultEntry mTxResultEntry;

    void verifyTxResultsForSingleLedger();
    void getTxResultSetForLedger(uint32_t seqNum);

  public:
    VerifyTxResultsWork(Application& app, WorkParent& parent,
                        TmpDir const& downloadDir, uint32_t checkpoint);
    ~VerifyTxResultsWork() override;
    Work::State onSuccess() override;
    void onStart() override;
    void onReset() override;
};
}
