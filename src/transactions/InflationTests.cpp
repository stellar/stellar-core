// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "util/Timer.h"
#include "main/Config.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerDelta.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "transactions/InflationOpFrame.h"
#include <functional>

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

static const int maxWinners = 50;

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
    SecretKey root = getRoot();

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    auto& lm = app.getLedgerManager();
    auto& db = app.getDatabase();

    int64 setupBalance = lm.getMinBalance(0);

    LedgerDelta delta(lm.getCurrentLedgerHeader());
    for (int i = 0; i < nbAccounts; i++)
    {
        int64 bal = getBalance(i);
        if (bal >= 0)
        {
            SecretKey to = getTestAccount(i);
            applyCreateAccountTx(app, root, to, rootSeq++, setupBalance);

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

    int64 minBalance = (totCoins * 15) / 1000; // 1.5%
    bool thresholdMode = false;

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
        if (v.second >= minBalance)
        {
            thresholdMode = true;
        }
    }

    // sort by votes, then by ID in descending order
    std::sort(votesV.begin(), votesV.end(),
              [](std::pair<int, int64> const& l, std::pair<int, int64> const& r)
              {
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

    if (!thresholdMode)
    {
        minBalance = 0;
    }

    std::vector<int> winners;
    int64 totVotes = 0;
    for (int i = 0; i < maxWinners && i < votesV.size(); i++)
    {
        if (votesV[i].second >= minBalance)
        {
            winners.emplace_back(votesV[i].first);
            totVotes += votesV[i].second;
        }
    }

    // 1% annual inflation on a weekly basis
    // 0.000190721
    int64 coinsToDole = bigDivide(totCoins, 190721, 1000000000);
    coinsToDole += totFees;
    totFees = 0;

    if (winners.empty())
    {
        // no winners -> accumulate in feepool
        totFees = coinsToDole;
    }
    else
    {
        for (auto w : winners)
        {
            // computes the share of this guy
            int64 toDoleToThis = bigDivide(coinsToDole, votes.at(w), totVotes);
            if (balances[w] >= 0)
            {
                balances[w] += toDoleToThis;
                totCoins += toDoleToThis;
            }
            else
            {
                totFees += toDoleToThis;
            }
        }
    }

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

    auto root = getRoot();
    TransactionFramePtr txFrame =
        createInflation(root, getAccountSeqNum(root, app) + 1);

    expectedFees += txFrame->getFee(app);

    expectedBalances =
        simulateInflation(nbAccounts, expectedTotcoins, expectedFees,
                          [&](int i)
                          {
                              return balances[i];
                          },
                          getVote);

    // perform actual inflation
    {
        LedgerDelta delta(lm.getCurrentLedgerHeader());
        REQUIRE(txFrame->apply(delta, app));
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

static time_t
getTestDate(int day, int month, int year)
{
    std::tm tm = {0};
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday = day;
    tm.tm_mon = month - 1; // 0 based
    tm.tm_year = year - 1900;

    VirtualClock::time_point tp = VirtualClock::tmToPoint(tm);
    time_t t = VirtualClock::to_time_t(tp);

    return t;
}

static void
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TransactionFramePtr tx = nullptr)
{
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);
    if (tx)
    {
        txSet->add(tx);
        txSet->sortForHash();
    }

    LedgerCloseData ledgerData(ledgerSeq, txSet, getTestDate(day, month, year),
                               10);
    app.getLedgerManager().closeLedger(ledgerData);

    REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));
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

    SecretKey root = getRoot();

    app.start();

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    SECTION("not time")
    {
        closeLedgerOn(app, 2, 30, 6, 2014);
        applyInflation(app, root, rootSeq++, INFLATION_NOT_TIME);

        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                0);

        closeLedgerOn(app, 3, 1, 7, 2014);

        auto txFrame = createInflation(root, rootSeq++);

        closeLedgerOn(app, 4, 7, 7, 2014, txFrame);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                1);

        applyInflation(app, root, rootSeq++, INFLATION_NOT_TIME);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                1);

        closeLedgerOn(app, 5, 8, 7, 2014);
        applyInflation(app, root, rootSeq++, INFLATION_SUCCESS);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                2);

        closeLedgerOn(app, 6, 14, 7, 2014);
        applyInflation(app, root, rootSeq++, INFLATION_NOT_TIME);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                2);

        closeLedgerOn(app, 7, 15, 7, 2014);
        applyInflation(app, root, rootSeq++, INFLATION_SUCCESS);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                3);

        closeLedgerOn(app, 8, 21, 7, 2014);
        applyInflation(app, root, rootSeq++, INFLATION_NOT_TIME);
        REQUIRE(app.getLedgerManager().getCurrentLedgerHeader().inflationSeq ==
                3);
    }
    // minVote to participate in inflation
    const int64 minVote = 1000000000LL;
    // 1.5% of all coins
    const int64 winnerVote = bigDivide(
        app.getLedgerManager().getCurrentLedgerHeader().totalCoins, 15, 1000);

    SECTION("inflation scenarios")
    {
        std::function<int(int)> voteFunc;
        std::function<int64(int)> balanceFunc;
        int nbAccounts = 0;
        int expectedWinners = 0;

        auto verify = [&]()
        {
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
            voteFunc = [&](int n)
            {
                return (n + 1) % nbAccounts;
            };
            balanceFunc = [&](int n)
            {
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
                expectedWinners = nbAccounts;
            }
            SECTION("more than max")
            {
                nbAccounts = 120;
                expectedWinners = maxWinners;
            }
            voteFunc = [&](int n)
            {
                return (n + 1) % nbAccounts;
            };
            balanceFunc = [&](int n)
            {
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
            voteFunc = [&](int n)
            {
                return 0;
            };
            balanceFunc = [&](int n)
            {
                return (n + 1) * minVote;
            };
            verify();
        }
        SECTION("50/50 split")
        {
            nbAccounts = 12;
            expectedWinners = 2;
            const int midPoint = nbAccounts / 2;

            const int64 each = bigDivide(winnerVote, 2, nbAccounts) + minVote;

            voteFunc = [&](int n)
            {
                return (n < midPoint) ? 0 : 1;
            };
            balanceFunc = [&](int n)
            {
                return each;
            };
            verify();
        }
        SECTION("no winner")
        {
            nbAccounts = 12;
            expectedWinners = 0;
            voteFunc = [&](int n)
            {
                return -1;
            };
            balanceFunc = [&](int n)
            {
                return (n + 1) * minVote;
            };
            verify();
        }
        SECTION("some winner does not exist")
        {
            nbAccounts = 13;
            expectedWinners = 1;
            const int midPoint = nbAccounts / 2;

            const int64 each = bigDivide(winnerVote, 2, nbAccounts) + minVote;

            voteFunc = [&](int n)
            {
                return (n < midPoint) ? 0 : 1;
            };
            balanceFunc = [&](int n)
            {
                // account "0" does not exist
                return (n == 0) ? -1 : each;
            };
            verify();
        }
    }
}
