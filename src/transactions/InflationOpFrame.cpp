// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/InflationOpFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "generated/StellarXDR.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

const uint32_t INFLATION_FREQUENCY = (60 * 60 * 24 * 7); // every 7 days
// inflation is .000190721 per 7 days, or 1% a year
const int64_t INFLATION_RATE_TRILLIONTHS = 190721000LL;
const int64_t TRILLION = 1000000000000LL;
const int64_t INFLATION_WIN_MIN_PERCENT = 15000000000LL; // 1.5%
const int INFLATION_NUM_WINNERS = 50;
const time_t INFLATION_START_TIME = (1404172800LL); // 1-jul-2014 (unix epoch)

namespace stellar
{
InflationOpFrame::InflationOpFrame(Operation const& op, OperationResult& res,
                                   TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

bool
InflationOpFrame::doApply(medida::MetricsRegistry& metrics,
                          LedgerDelta& delta, LedgerManager& ledgerManager)
{
    LedgerDelta inflationDelta(delta);

    auto& lcl = inflationDelta.getHeader();

    time_t closeTime = lcl.closeTime;
    uint32_t seq = lcl.inflationSeq;

    time_t inflationTime = (INFLATION_START_TIME + seq * INFLATION_FREQUENCY);
    if (closeTime < inflationTime)
    {
        metrics.NewMeter({"op-inflation", "failure", "not-time"},
                         "operation").Mark();
        innerResult().code(INFLATION_NOT_TIME);
        return false;
    }

    /*
    Inflation is calculated using the following

    1. calculate tally of votes based on "inflationDest" set on each account
    2. take the top accounts (by vote) that exceed 1.5%
        (INFLATION_WIN_MIN_PERCENT) of votes,
        up to 50 accounts (INFLATION_NUM_WINNERS)
    exception:
    if no account crosses the INFLATION_WIN_MIN_PERCENT, the top 50 is used
    3. share the coins between those accounts proportionally to the number
        of votes they got.
    */

    int64_t totalVotes = 0;
    bool first = true;
    int64_t minBalance =
        bigDivide(lcl.totalCoins, INFLATION_WIN_MIN_PERCENT, TRILLION);

    std::vector<AccountFrame::InflationVotes> winners;
    auto& db = ledgerManager.getDatabase();

    AccountFrame::processForInflation(
        [&](AccountFrame::InflationVotes const& votes)
        {
            if (first && votes.mVotes < minBalance)
            {
                // need to take the entire set if nobody crossed the threshold
                minBalance = 0;
            }

            first = false;

            bool res;

            if (votes.mVotes >= minBalance)
            {
                totalVotes += votes.mVotes;
                winners.push_back(votes);
                res = true;
            }
            else
            {
                res = false;
            }

            return res;
        },
        INFLATION_NUM_WINNERS, db);

    int64 amountToDole =
        bigDivide(lcl.totalCoins, INFLATION_RATE_TRILLIONTHS, TRILLION);
    amountToDole += lcl.feePool;

    lcl.feePool = 0;
    lcl.inflationSeq++;

    // now credit each account
    innerResult().code(INFLATION_SUCCESS);
    auto& payouts = innerResult().payouts();

    if (totalVotes != 0)
    {
        for (auto const& w : winners)
        {
            AccountFrame::pointer winner;

            int64 toDoleThisWinner =
                bigDivide(amountToDole, w.mVotes, totalVotes);

            if (toDoleThisWinner == 0)
                continue;

            winner = AccountFrame::loadAccount(w.mInflationDest, db);

            if (winner)
            {
                lcl.totalCoins += toDoleThisWinner;
                winner->getAccount().balance += toDoleThisWinner;
                winner->storeChange(inflationDelta, db);
                payouts.emplace_back(w.mInflationDest, toDoleThisWinner);
            }
            else
            {
                // put back in fee pool as unclaimed funds
                lcl.feePool += toDoleThisWinner;
            }
        }
    }
    else
    {
        // put back in fee pool as unclaimed funds
        lcl.feePool += amountToDole;
    }

    inflationDelta.commit();

    metrics.NewMeter({"op-inflation", "success", "apply"},
                     "operation").Mark();
    return true;
}

bool
InflationOpFrame::doCheckValid(medida::MetricsRegistry& metrics)
{
    return true;
}

int32_t
InflationOpFrame::getNeededThreshold() const
{
    return mSourceAccount->getLowThreshold();
}
}
