// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetFrame.h"
#include "util/XDRStream.h"

namespace stellar
{
class Application;

/*
 * CheckpointBuilder manages the ACID transactional appending of confirmed
ledgers to sequential streams (transaction results, transactions, and ledger
headers) and the ACID queueing of completed checkpoints for historical purposes.
Completed checkpoints are published to history archives by PublihsWork class.

* Atomicity is ensured through a write-tmp-file-then-rename cycle, where files
are first written as temporary "dirty" files and then renamed to their final
names to mark a successfully constructed checkpoint. Note that tmp files are
highly durable, and are fsynced on every write. This way on crash all publish
data is preserved and can be recovered to a valid checkpoint on restart.

* All publish files are renamed to their final names _after_ ledger commits.
This ensures that final checkpoint files are always valid (and do not contain
uncommitted data). If a crash occurs before these files are finalized, but after
commit, core will finalize files on restart based on LCL.

* Because of the requirement above, we can derive the following guarantees:
  - Dirty publish files must always end at a ledger that is greater than or
equal to LCL in the database.
  - Final publish files must always end at a ledger that is less than or equal
to LCL in the database.

* Both publish queue and checkpoint files rely on the last committed ledger
sequence stored in SQL (LCL). If a crash or shutdown occurs, publish file state
may be left inconsistent with the DB. On restart, core automatically recovers
valid publish state based on the LCL. For publish queue files, this means
deleting any checkpoints with ledger sequence greater than LCL. For the rest of
the files, this means extracting a history prefix of a checkpoint up until and
including the LCL, and truncating the rest (including malformed data). If a
crash before commit occurrs on a first ledger in a checkpoint, the dirty file is
simply deleted on startup. This ensures core always starts with a valid publish
state.
 */
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
    bool mPublishWasDisabled{false};

    bool ensureOpen(uint32_t ledgerSeq);

  public:
    CheckpointBuilder(Application& app);
    void appendTransactionSet(uint32_t ledgerSeq,
                              TxSetXDRFrameConstPtr const& txSet,
                              TransactionResultSet const& resultSet,
                              bool skipStartupCheck = false);
    void appendTransactionSet(uint32_t ledgerSeq,
                              TransactionHistoryEntry const& txSet,
                              TransactionResultSet const& resultSet,
                              bool skipStartupCheck = false);
    void appendLedgerHeader(LedgerHeader const& header,
                            bool skipStartupCheck = false);

    // Cleanup publish files according to the latest LCL.
    // Publish files might contain dirty data if a crash occurred after append
    // but before commit in LedgerManagerImpl::closeLedger
    void cleanup(uint32_t lcl);

    // Finalize checkpoint by renaming all temporary files to their canonical
    // names. No-op if files are already rotated.
    void checkpointComplete(uint32_t checkpoint);
};
}