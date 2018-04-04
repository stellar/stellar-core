#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include <vector>

namespace stellar
{

class Application;
class LedgerState;

namespace InvariantTestUtils
{

LedgerEntry generateRandomAccount(uint32_t ledgerSeq);

typedef std::vector<std::tuple<LedgerEntry const*, LedgerEntry const*>>
    UpdateList;

bool store(Application& app, UpdateList const& apply,
           LedgerState* lsPtr = nullptr,
           OperationResult const* resPtr = nullptr);

UpdateList makeUpdateList(LedgerEntry const* left, LedgerEntry const* right);
}
}
