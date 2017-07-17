// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/Progress.h"
#include "history/HistoryManager.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{

std::string
fmtProgress(Application& app, std::string const& task, uint32_t first,
            uint32_t last, uint32_t curr)
{
    auto step = app.getHistoryManager().getCheckpointFrequency();
    if (curr > last)
    {
        curr = last;
    }
    if (curr < first)
    {
        curr = first;
    }
    if (step == 0)
    {
        step = 1;
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
