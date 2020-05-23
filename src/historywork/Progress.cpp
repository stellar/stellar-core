// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/Progress.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerRange.h"
#include "main/Application.h"
#include <fmt/format.h>

namespace stellar
{

std::string
fmtProgress(Application& app, std::string const& task, LedgerRange const& range,
            uint32_t curr)
{
    auto step = app.getHistoryManager().getCheckpointFrequency();
    // Step is only ever 8 or 64.
    assert(step != 0);
    if (range.mCount == 0)
    {
        return fmt::format("{:s} 0/0 (100%)", task);
    }
    auto first = range.mFirst;
    auto last = range.last();
    if (curr > last)
    {
        curr = last;
    }
    if (curr < first)
    {
        curr = first;
    }
    if (last < first)
    {
        last = first;
    }
    auto done = 1 + ((curr - first) / step);
    auto total = 1 + ((last - first) / step);
    auto pct = (100 * done) / total;
    return fmt::format("{:s} {:d}/{:d} ({:d}%)", task, done, total, pct);
}
}
