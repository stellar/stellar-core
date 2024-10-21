// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "database/Database.h"
#include "overlay/StellarXDR.h"

namespace stellar
{
class Application;
class CheckpointBuilder;

size_t populateCheckpointFilesFromDB(Application& app, soci::session& sess,
                                     uint32_t ledgerSeq, uint32_t ledgerCount,
                                     CheckpointBuilder& checkpointBuilder);
void dropSupportTransactionFeeHistory(Database& db);
void dropSupportTxSetHistory(Database& db);
void dropSupportTxHistory(Database& db);
}
