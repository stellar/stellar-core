#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <vector>

namespace stellar
{

class Application;

namespace InvariantTestUtils
{

LedgerEntry generateRandomAccount(uint32_t ledgerSeq);

typedef std::vector<std::tuple<EntryFrame::pointer, EntryFrame::pointer>>
    UpdateList;

bool store(Application& app, UpdateList const& apply,
           LedgerDelta* ldPtr = nullptr,
           OperationResult const* resPtr = nullptr);

UpdateList makeUpdateList(EntryFrame::pointer left, EntryFrame::pointer right);
}
}
