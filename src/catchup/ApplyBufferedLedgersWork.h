// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/LedgerCloseData.h"
#include "work/BasicWork.h"
#include "work/ConditionalWork.h"

namespace stellar
{

class ApplyBufferedLedgersWork : public BasicWork
{
    using OnSuccessHandler =
        std::function<void(LedgerHeaderHistoryEntry const& lastClosed)>;
    const OnSuccessHandler mOnSuccessHandler;
    std::shared_ptr<ConditionalWork> mConditionalWork;

  public:
    ApplyBufferedLedgersWork(Application& app,
                             OnSuccessHandler const& onSuccessHandler);

  protected:
    void onReset() override;
    State onRun() override;
    void onSuccess() override;
    bool
    onAbort() override
    {
        return true;
    };
};
}