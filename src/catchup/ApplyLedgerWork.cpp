// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyLedgerWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{
ApplyLedgerWork::ApplyLedgerWork(Application& app,
                                 LedgerCloseData const& ledgerCloseData)
    : BasicWork(
          app, "apply-ledger-" + std::to_string(ledgerCloseData.getLedgerSeq()),
          BasicWork::RETRY_NEVER)
    , mLedgerCloseData(ledgerCloseData)
{
}

BasicWork::State
ApplyLedgerWork::onRun()
{
    ZoneScoped;
    mApp.getLedgerManager().closeLedger(mLedgerCloseData);
    return BasicWork::State::WORK_SUCCESS;
}

bool
ApplyLedgerWork::onAbort()
{
    return true;
}

std::string
ApplyLedgerWork::getStatus() const
{
    return fmt::format("apply ledger {}", mLedgerCloseData.getLedgerSeq());
}
}
