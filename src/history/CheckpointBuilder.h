// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "util/XDRStream.h"

namespace stellar
{
class Application;

class CheckpointBuilder
{
    Application& mApp;
#ifdef BUILD_TESTS
  public:
#endif
    // Current checkpoint streams (unpublished data)
    std::unique_ptr<XDROutputFileStream> mTxResults;
    std::unique_ptr<XDROutputFileStream> mTxs;
    std::unique_ptr<XDROutputFileStream> mLedgerHeaders;
    bool mOpen{false};
    bool mStartupValidationComplete{false};

    void maybeOpen(uint32_t ledgerSeq);

  public:
    CheckpointBuilder(Application& app);
    void appendTransactionSet(uint32_t ledgerSeq,
                              TxSetXDRFrameConstPtr const& txSet,
                              TransactionResultSet const& resultSet);
    void appendLedgerHeader(LedgerHeader const& header);

    // Cleanup publish files according to the latest LCL.
    // Publish files might contain dirty data if a crash occurred after append
    // but before commit in LedgerManagerImpl::closeLedger
    void cleanup(uint32_t lcl);

    // Finalize checkpoint by renaming all temporary files to their canonical
    // names. No-op if files are already rotated.
    void checkpointComplete(uint32_t checkpoint);
};
}