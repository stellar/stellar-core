#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkSequence.h"

namespace stellar
{
class GetHistoryArchiveStateWork;
class HistoryArchive;
class HistoryArchiveReportWork : public WorkSequence
{
  protected:
    void onFailureRaise() override;
    void onSuccess() override;

  public:
    HistoryArchiveReportWork(
        Application& app,
        std::vector<std::shared_ptr<GetHistoryArchiveStateWork>> const& works);
    ~HistoryArchiveReportWork() = default;

  private:
    std::vector<std::shared_ptr<GetHistoryArchiveStateWork>>
        mGetHistoryArchiveStateWorks;
};
}
