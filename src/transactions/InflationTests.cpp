// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/LedgerCloseData.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/InflationOpFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include <functional>

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

static const unsigned maxWinners = 2000u;

static SecretKey
getTestAccount(int i)
{
    std::stringstream name;
    name << "A" << i;
    return getAccount(name.str().c_str());
}

static void
createTestAccounts(Application& app, int nbAccounts,
                   std::function<int64(int)> getBalance,
                   std::function<int(int)> getVote)
{
    // set up world
    auto root = TestAccount::createRoot(app);

    auto& lm = app.getLedgerManager();
    auto& db = app.getDatabase();

    int64 setupBalance = lm.getMinBalance(0);

    LedgerDelta delta(lm.getCurrentLedgerHeader(), app.getDatabase());
    for (int i = 0; i < nbAccounts; i++)
    {
        int64 bal = getBalance(i);
        if (bal >= 0)
        {
            SecretKey to = getTestAccount(i);
            root.create(to, setupBalance);

            AccountFrame::pointer act;
            act = loadAccount(to, app);
            act->getAccount().balance = bal;
            act->getAccount().inflationDest.activate() =
                getTestAccount(getVote(i)).getPublicKey();
            act->storeChange(delta, db);
        }
    }
}

// computes the resulting balance of each test account
static std::vector<int64>
simulateInflation(int nbAccounts, int64& totCoins, int64& totFees,
                  std::function<int64(int)> getBalance,
                  std::function<int(int)> getVote)
{
    std::map<int, int64> balances;
    std::map<int, int64> votes;

    std::vector<std::pair<int, int64>> votesV;

    int64 minBalance = (totCoins * 5) / 10000; // .05%

    // computes all votes
    for (int i = 0; i < nbAccounts; i++)
    {
        int64 bal = getBalance(i);
        balances[i] = bal;
        // negative balance means the account does not exist
        if (bal >= 0)
        {
            int vote = getVote(i);
            // negative means inflationdest is not set for this account
            if (vote >= 0)
            {
                votes[vote] += bal;
            }
        }
    }

    for (auto const& v : votes)
    {
        votesV.emplace_back(v);
    }

    // sort by votes, then by ID in descending order
    std::sort(votesV.begin(), votesV.end(), [](std::pair<int, int64> const& l,
                                               std::pair<int, int64> const& r) {
        if (l.second > r.second)
        {
            return true;
        }
        else if (l.second < r.second)
        {
            return false;
        }
        else
        {
            return l.first > r.first;
        }
    });

    std::vector<int> winners;
    int64 totVotes = totCoins;
    for (size_t i = 0u; i < maxWinners && i < votesV.size(); i++)
    {
        if (votesV[i].second >= minBalance)
        {
            winners.emplace_back(votesV[i].first);
        }
    }

    // 1% annual inflation on a weekly basis
    // 0.000190721
    int64 coinsToDole = bigDivide(totCoins, 190721, 1000000000, ROUND_DOWN);
    coinsToDole += totFees;
    int64 leftToDole = coinsToDole;

    for (auto w : winners)
    {
        // computes the share of this guy
        int64 toDoleToThis =
            bigDivide(coinsToDole, votes.at(w), totVotes, ROUND_DOWN);
        if (balances[w] >= 0)
        {
            balances[w] += toDoleToThis;
            totCoins += toDoleToThis;
            leftToDole -= toDoleToThis;
        }
    }

    totFees = leftToDole;

    std::vector<int64> balRes;
    for (auto const& b : balances)
    {
        balRes.emplace_back(b.second);
    }
    return balRes;
}

static void
doInflation(Application& app, int nbAccounts,
            std::function<int64(int)> getBalance,
            std::function<int(int)> getVote, int expectedWinnerCount)
{
    using xdr::operator==;

    // simulate the expected inflation based off the current ledger state
    std::map<int, int64> balances;

    // load account balances
    for (int i = 0; i < nbAccounts; i++)
    {
        if (getBalance(i) < 0)
        {
            balances[i] = -1;
            requireNoAccount(getTestAccount(i), app);
        }
        else
        {
            AccountFrame::pointer act;
            act = loadAccount(getTestAccount(i), app);
            balances[i] = act->getBalance();
            // double check that inflationDest is setup properly
            if (act->getAccount().inflationDest)
            {
                REQUIRE(getTestAccount(getVote(i)).getPublicKey() ==
                        *act->getAccount().inflationDest);
            }
            else
            {
                REQUIRE(getVote(i) < 0);
            }
        }
    }
    LedgerManager& lm = app.getLedgerManager();
    LedgerHeader& cur = lm.getCurrentLedgerHeader();
    cur.feePool = 10000;

    int64 expectedTotcoins = cur.totalCoins;
    int64 expectedFees = cur.feePool;

    std::vector<int64> expectedBalances;

    auto root = TestAccount::createRoot(app);
    TransactionFramePtr txFrame =
        createInflation(app.getNetworkID(), root, root.nextSequenceNumber());

    expectedFees += txFrame->getFee();

    expectedBalances =
        simulateInflation(nbAccounts, expectedTotcoins, expectedFees,
                          [&](int i) { return balances[i]; }, getVote);

    // perform actual inflation
    {
        LedgerDelta delta(lm.getCurrentLedgerHeader(), app.getDatabase());
        REQUIRE(applyCheck(txFrame, delta, app));
        delta.commit();
    }

    // verify ledger state
    LedgerHeader& cur2 = lm.getCurrentLedgerHeader();

    REQUIRE(cur2.totalCoins == expectedTotcoins);
    REQUIRE(cur2.feePool == expectedFees);

    // verify balances
    InflationResult const& infResult =
        getFirstResult(*txFrame).tr().inflationResult();
    auto const& payouts = infResult.payouts();
    int actualChanges = 0;

    for (int i = 0; i < nbAccounts; i++)
    {
        auto const& k = getTestAccount(i);
        if (expectedBalances[i] < 0)
        {
            requireNoAccount(k, app);
            REQUIRE(balances[i] < 0); // account didn't get deleted
        }
        else
        {
            AccountFrame::pointer act;
            act = loadAccount(k, app);
            REQUIRE(expectedBalances[i] == act->getBalance());

            if (expectedBalances[i] != balances[i])
            {
                REQUIRE(balances[i] >= 0);
                actualChanges++;
                bool found = false;
                for (auto const& p : payouts)
                {
                    if (p.destination == k.getPublicKey())
                    {
                        int64 computedFromResult = balances[i] + p.amount;
                        REQUIRE(computedFromResult == expectedBalances[i]);
                        found = true;
                        break;
                    }
                }
                REQUIRE(found);
            }
        }
    }
    REQUIRE(actualChanges == expectedWinnerCount);
    REQUIRE(expectedWinnerCount == payouts.size());
}

TEST_CASE("inflation", "[tx][inflation]")
{
    Config const& cfg = getTestConfig(0);

    VirtualClock::time_point inflationStart;
    // inflation starts on 1-jul-2014
    time_t start = getTestDate(1, 7, 2014);
    inflationStart = VirtualClock::from_time_t(start);

    VirtualClock clock;
    clock.setCurrentTime(inflationStart);

    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    auto root = TestAccount::createRoot(app);

    app.start();

    SECTION("not time")
    {
        closeLedgerOn(app, 2, 30, 6, 2014);
        applyInflation(app, root, root.nextSequenceNumber(),
                       INFLATION_NOT_TIME);

        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                0);

        closeLedgerOn(app, 3, 1, 7, 2014);

        auto txFrame = createInflation(app.getNetworkID(), root,
                                       root.nextSequenceNumber());

        closeLedgerOn(app, 4, 7, 7, 2014, txFrame);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                1);

        applyInflation(app, root, root.nextSequenceNumber(),
                       INFLATION_NOT_TIME);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                1);

        closeLedgerOn(app, 5, 8, 7, 2014);
        applyInflation(app, root, root.nextSequenceNumber(), INFLATION_SUCCESS);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                2);

        closeLedgerOn(app, 6, 14, 7, 2014);
        applyInflation(app, root, root.nextSequenceNumber(),
                       INFLATION_NOT_TIME);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                2);

        closeLedgerOn(app, 7, 15, 7, 2014);
        applyInflation(app, root, root.nextSequenceNumber(), INFLATION_SUCCESS);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                3);

        closeLedgerOn(app, 8, 21, 7, 2014);
        applyInflation(app, root, root.nextSequenceNumber(),
                       INFLATION_NOT_TIME);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                3);
    }
    // minVote to participate in inflation
    const int64 minVote = 1000000000LL;
    // .05% of all coins
    const int64 winnerVote =
        bigDivide(app.getLedgerManager().getCurrentLedgerHeader().totalCoins, 5,
                  10000, ROUND_DOWN);

    SECTION("inflation scenarios")
    {
        std::function<int(int)> voteFunc;
        std::function<int64(int)> balanceFunc;
        int nbAccounts = 0;
        int expectedWinners = 0;

        auto verify = [&]() {
            if (nbAccounts != 0)
            {
                createTestAccounts(app, nbAccounts, balanceFunc, voteFunc);
                closeLedgerOn(app, 2, 21, 7, 2014);

                doInflation(app, nbAccounts, balanceFunc, voteFunc,
                            expectedWinners);
            }
        };

        SECTION("two guys over threshold")
        {
            nbAccounts = 120;
            expectedWinners = 2;
            voteFunc = [&](int n) { return (n + 1) % nbAccounts; };
            balanceFunc = [&](int n) {
                if (n == 0 || n == 5)
                {
                    return winnerVote;
                }
                else
                {
                    return minVote;
                }
            };
            verify();
        }
        SECTION("no one over min")
        {
            SECTION("less than max")
            {
                nbAccounts = 12;
                expectedWinners = 0;
            }
            SECTION("more than max")
            {
                nbAccounts = 2200;
                expectedWinners = 0;
            }
            voteFunc = [&](int n) { return (n + 1) % nbAccounts; };
            balanceFunc = [&](int n) {
                int64 balance = (n + 1) * minVote;
                assert(balance < winnerVote);
                return balance;
            };
            verify();
        }
        SECTION("all to one destination")
        {
            nbAccounts = 12;
            expectedWinners = 1;
            voteFunc = [&](int n) { return 0; };
            balanceFunc = [&](int n) { return 1 + (winnerVote / nbAccounts); };
            verify();
        }
        SECTION("50/50 split")
        {
            nbAccounts = 12;
            expectedWinners = 2;
            const int midPoint = nbAccounts / 2;

            const int64 each =
                bigDivide(winnerVote, 2, nbAccounts, ROUND_DOWN) + minVote;

            voteFunc = [&](int n) { return (n < midPoint) ? 0 : 1; };
            balanceFunc = [&](int n) { return each; };
            verify();
        }
        SECTION("no winner")
        {
            nbAccounts = 12;
            expectedWinners = 0;
            voteFunc = [&](int n) { return -1; };
            balanceFunc = [&](int n) { return (n + 1) * minVote; };
            verify();
        }
        SECTION("some winner does not exist")
        {
            nbAccounts = 13;
            expectedWinners = 1;
            const int midPoint = nbAccounts / 2;

            const int64 each =
                bigDivide(winnerVote, 2, nbAccounts, ROUND_DOWN) + minVote;

            voteFunc = [&](int n) { return (n < midPoint) ? 0 : 1; };
            balanceFunc = [&](int n) {
                // account "0" does not exist
                return (n == 0) ? -1 : each;
            };
            verify();
        }
    }
}
