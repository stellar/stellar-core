// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/InflationOpFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/StellarXDR.h"

const uint32_t INFLATION_FREQUENCY = (60 * 60 * 24 * 7); // every 7 days
// inflation is .000190721 per 7 days, or 1% a year
const int64_t INFLATION_RATE_TRILLIONTHS = 190721000LL;
const int64_t TRILLION = 1000000000000LL;
const int64_t INFLATION_WIN_MIN_PERCENT = 500000000LL; // .05%
const int INFLATION_NUM_WINNERS = 2000;
const time_t INFLATION_START_TIME = (1404172800LL); // 1-jul-2014 (unix epoch)

namespace stellar
{
InflationOpFrame::InflationOpFrame(Operation const& op, OperationResult& res,
                                   TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

bool
InflationOpFrame::doApply(Application& app, LedgerDelta& delta,
                          LedgerManager& ledgerManager)
{
    LedgerDelta inflationDelta(delta);

    auto& lcl = inflationDelta.getHeader();

    time_t closeTime = lcl.scpValue.closeTime;
    uint64_t seq = lcl.inflationSeq;

    time_t inflationTime = (INFLATION_START_TIME + seq * INFLATION_FREQUENCY);
    if (closeTime < inflationTime)
    {
        app.getMetrics()
            .NewMeter({"op-inflation", "failure", "not-time"}, "operation")
            .Mark();
        innerResult().code(INFLATION_NOT_TIME);
        return false;
    }

    /*
    Inflation is calculated using the following

    1. calculate tally of votes based on "inflationDest" set on each account
    2. take the top accounts (by vote) that get at least .05% of the vote
    3. If no accounts are over this threshold then the extra goes back to the
       inflation pool
    */

    int64_t totalVotes = lcl.totalCoins;
    int64_t minBalance =
        bigDivide(totalVotes, INFLATION_WIN_MIN_PERCENT, TRILLION, ROUND_DOWN);

    std::vector<AccountFrame::InflationVotes> winners;
    auto& db = ledgerManager.getDatabase();

    AccountFrame::processForInflation(
        [&](AccountFrame::InflationVotes const& votes) {
            if (votes.mVotes >= minBalance)
            {
                winners.push_back(votes);
                return true;
            }
            return false;
        },
        INFLATION_NUM_WINNERS, db);

    auto inflationAmount = bigDivide(lcl.totalCoins, INFLATION_RATE_TRILLIONTHS,
                                     TRILLION, ROUND_DOWN);
    auto amountToDole = inflationAmount + lcl.feePool;

    lcl.feePool = 0;
    lcl.inflationSeq++;

    // now credit each account
    innerResult().code(INFLATION_SUCCESS);
    auto& payouts = innerResult().payouts();

    int64 leftAfterDole = amountToDole;

    for (auto const& w : winners)
    {
        AccountFrame::pointer winner;

        int64 toDoleThisWinner =
            bigDivide(amountToDole, w.mVotes, totalVotes, ROUND_DOWN);

        if (toDoleThisWinner == 0)
            continue;

        winner =
            AccountFrame::loadAccount(inflationDelta, w.mInflationDest, db);

        if (winner)
        {
            leftAfterDole -= toDoleThisWinner;
            if (ledgerManager.getCurrentLedgerVersion() <= 7)
            {
                lcl.totalCoins += toDoleThisWinner;
            }
            if (!winner->addBalance(toDoleThisWinner))
            {
                throw std::runtime_error(
                    "inflation overflowed destination balance");
            }
            winner->storeChange(inflationDelta, db);
            payouts.emplace_back(w.mInflationDest, toDoleThisWinner);
        }
    }

    // put back in fee pool as unclaimed funds
    lcl.feePool += leftAfterDole;
    if (ledgerManager.getCurrentLedgerVersion() > 7)
    {
        lcl.totalCoins += inflationAmount;
    }

    inflationDelta.commit();

    app.getMetrics()
        .NewMeter({"op-inflation", "success", "apply"}, "operation")
        .Mark();
    return true;
}

bool
InflationOpFrame::doCheckValid(Application& app)
{
    return true;
}

ThresholdLevel
InflationOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}
}
