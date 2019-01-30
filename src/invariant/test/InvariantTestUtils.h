#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <vector>

namespace stellar
{

class Application;
class AbstractLedgerTxn;
struct LedgerEntry;
struct OperationResult;

namespace InvariantTestUtils
{

LedgerEntry generateRandomAccount(uint32_t ledgerSeq);

typedef std::vector<
    std::tuple<std::shared_ptr<LedgerEntry>, std::shared_ptr<LedgerEntry>>>
    UpdateList;

bool store(Application& app, UpdateList const& apply,
           AbstractLedgerTxn* ltxPtr = nullptr,
           OperationResult const* resPtr = nullptr);

UpdateList makeUpdateList(std::vector<LedgerEntry> const& current,
                          std::nullptr_t previous);
UpdateList makeUpdateList(std::vector<LedgerEntry> const& current,
                          std::vector<LedgerEntry> const& previous);
UpdateList makeUpdateList(std::nullptr_t current,
                          std::vector<LedgerEntry> const& previous);
}
}
