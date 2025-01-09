// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyBufferedLedgersWork.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucketList.h"
#include "catchup/ApplyLedgerWork.h"
#include "crypto/Hex.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{
ApplyBufferedLedgersWork::ApplyBufferedLedgersWork(Application& app)
    : BasicWork(app, "apply-buffered-ledgers", BasicWork::RETRY_NEVER)
{
}

void
ApplyBufferedLedgersWork::onReset()
{
    mConditionalWork.reset();
}

BasicWork::State
ApplyBufferedLedgersWork::onRun()
{
    ZoneScoped;
    if (mConditionalWork)
    {
        mConditionalWork->crankWork();
        if (mConditionalWork->getState() != State::WORK_SUCCESS)
        {
            return mConditionalWork->getState();
        }
    }

    std::optional<LedgerCloseData> maybeLcd =
        mApp.getLedgerApplyManager().maybeGetNextBufferedLedgerToApply();

    if (!maybeLcd)
    {
        CLOG_INFO(History, "No more buffered ledgers to apply");
        return State::WORK_SUCCESS;
    }
    auto const& lcd = maybeLcd.value();

    CLOG_INFO(
        History,
        "Scheduling buffered ledger-close: [seq={}, prev={}, txs={}, "
        "ops={}, sv: {}]",
        lcd.getLedgerSeq(), hexAbbrev(lcd.getTxSet()->previousLedgerHash()),
        lcd.getTxSet()->sizeTxTotal(), lcd.getTxSet()->sizeOpTotalForLogging(),
        stellarValueToString(mApp.getConfig(), lcd.getValue()));

    auto applyLedger = std::make_shared<ApplyLedgerWork>(mApp, lcd);

    auto predicate = [](Application& app) {
        auto& bl = app.getBucketManager().getLiveBucketList();
        auto& lm = app.getLedgerManager();
        bl.resolveAnyReadyFutures();
        return bl.futuresAllResolved(
            bl.getMaxMergeLevel(lm.getLastClosedLedgerNum() + 1));
    };

    mConditionalWork = std::make_shared<ConditionalWork>(
        mApp,
        fmt::format(
            FMT_STRING("apply-buffered-ledger-conditional ledger({:d})"),
            lcd.getLedgerSeq()),
        predicate, applyLedger, std::chrono::milliseconds(500));

    mConditionalWork->startWork(wakeSelfUpCallback());

    return State::WORK_RUNNING;
}

std::string
ApplyBufferedLedgersWork::getStatus() const
{
    return fmt::format(FMT_STRING("Applying buffered ledgers: {}"),
                       mConditionalWork ? mConditionalWork->getStatus()
                                        : BasicWork::getStatus());
}

void
ApplyBufferedLedgersWork::shutdown()
{
    ZoneScoped;
    if (mConditionalWork)
    {
        mConditionalWork->shutdown();
    }
    BasicWork::shutdown();
}

bool
ApplyBufferedLedgersWork::onAbort()
{
    ZoneScoped;
    if (mConditionalWork && !mConditionalWork->isDone())
    {
        mConditionalWork->crankWork();
        return false;
    }
    return true;
}
}
