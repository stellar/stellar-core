// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryArchiveReportWork.h"
#include "history/HistoryArchive.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "util/Logging.h"
#include "work/WorkSequence.h"
#include <Tracy.hpp>
#include <fmt/format.h>
#include <iostream>

namespace stellar
{

HistoryArchiveReportWork::HistoryArchiveReportWork(
    Application& app,
    std::vector<std::shared_ptr<GetHistoryArchiveStateWork>> const& works)
    : WorkSequence(
          app, "history-archive-report-work",
          std::vector<std::shared_ptr<BasicWork>>(works.begin(), works.end()),
          BasicWork::RETRY_NEVER)
    , mGetHistoryArchiveStateWorks(works)
{
}

void
HistoryArchiveReportWork::onSuccess()
{
    for (auto const& work : mGetHistoryArchiveStateWorks)
    {
        CLOG_INFO(History,
                  "Archive information: [name: {}, server: {}, "
                  "currentLedger: "
                  "{}]",
                  work->getArchive()->getName(),
                  work->getHistoryArchiveState().server,
                  work->getHistoryArchiveState().currentLedger);
    }
}

void
HistoryArchiveReportWork::onFailureRaise()
{
    for (auto const& work : mGetHistoryArchiveStateWorks)
    {
        if (work->getState() != State::WORK_SUCCESS)
        {
            CLOG_ERROR(History, "Failed to obtain archive information: {}",
                       work->getArchive()->getName());
            return;
        }
    }
}
}
