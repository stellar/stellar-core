// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/LedgerCloseData.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include <functional>

using namespace stellar;
using namespace stellar::txtest;

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

    for (int i = 0; i < nbAccounts; i++)
    {
        int64 bal = getBalance(i);
        if (bal >= 0)
        {
            SecretKey to = getTestAccount(i);
            root.create(to, bal);

            LedgerState ls(app.getLedgerStateRoot());
            auto account = stellar::loadAccount(ls, to.getPublicKey());
            account.account().inflationDest.activate() =
                getTestAccount(getVote(i)).getPublicKey();
            ls.commit();
        }
    }
}

// computes the resulting balance of each test account
static std::vector<int64>
simulateInflation(int ledgerVersion, int nbAccounts, int64& totCoins,
                  int64& totFees, std::function<int64(int)> getBalance,
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
    std::sort(
        votesV.begin(), votesV.end(),
        [](std::pair<int, int64> const& l, std::pair<int, int64> const& r) {
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
    auto inflation = bigDivide(totCoins, 190721, 1000000000, ROUND_DOWN);
    auto coinsToDole = inflation + totFees;
    int64 leftToDole = coinsToDole;

    for (auto w : winners)
    {
        // computes the share of this guy
        int64 toDoleToThis =
            bigDivide(coinsToDole, votes.at(w), totVotes, ROUND_DOWN);
        if (balances[w] >= 0)
        {
            balances[w] += toDoleToThis;
            if (ledgerVersion <= 7)
            {
                totCoins += toDoleToThis;
            }
            leftToDole -= toDoleToThis;
        }
    }

    if (ledgerVersion > 7)
    {
        totCoins += inflation;
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
doInflation(Application& app, int ledgerVersion, int nbAccounts,
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
            REQUIRE(!hasAccount(app, getTestAccount(i).getPublicKey()));
        }
        else
        {
            LedgerState ls(app.getLedgerStateRoot());
            auto act = stellar::loadAccount(ls, getTestAccount(i).getPublicKey());
            REQUIRE(act);
            balances[i] = act.getBalance();
            // double check that inflationDest is setup properly
            if (act.account().inflationDest)
            {
                REQUIRE(getTestAccount(getVote(i)).getPublicKey() ==
                        *act.account().inflationDest);
            }
            else
            {
                REQUIRE(getVote(i) < 0);
            }
        }
    }

    LedgerState ls1(app.getLedgerStateRoot());
    auto header = ls1.loadHeader();
    REQUIRE(header->header().feePool > 0);
    int64 expectedTotcoins = header->header().totalCoins;
    int64 expectedFees = header->header().feePool;
    ls1.rollback();

    std::vector<int64> expectedBalances;

    auto root = TestAccount::createRoot(app);
    auto txFrame = root.tx({inflation()});
    expectedFees += txFrame->getFee();

    expectedBalances = simulateInflation(
        ledgerVersion, nbAccounts, expectedTotcoins, expectedFees,
        [&](int i) { return balances[i]; }, getVote);

    // perform actual inflation
    applyTx(txFrame, app);

    // verify ledger state
    LedgerState ls2(app.getLedgerStateRoot());
    header = ls2.loadHeader();
    REQUIRE(header->header().totalCoins == expectedTotcoins);
    REQUIRE(header->header().feePool == expectedFees);
    ls2.rollback();

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
            REQUIRE(!hasAccount(app, k.getPublicKey()));
            REQUIRE(balances[i] < 0); // account didn't get deleted
        }
        else
        {
            LedgerState ls(app.getLedgerStateRoot());
            auto act = stellar::loadAccount(ls, k.getPublicKey());
            REQUIRE(act);
            REQUIRE(expectedBalances[i] == act.getBalance());

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

    auto app = createTestApplication(clock, cfg);

    auto root = TestAccount::createRoot(*app);

    app->start();

    auto getInflationSeq = [&app] () {
        LedgerState ls(app->getLedgerStateRoot());
        return ls.loadHeader()->header().inflationSeq;
    };
    auto getFeePool = [&app] () {
        LedgerState ls(app->getLedgerStateRoot());
        return ls.loadHeader()->header().feePool;
    };
    auto getTotalCoins = [&app] () {
        LedgerState ls(app->getLedgerStateRoot());
        return ls.loadHeader()->header().totalCoins;
    };

    SECTION("not time")
    {
        for_all_versions(*app, [&] {
            closeLedgerOn(*app, 2, 30, 6, 2014);
            REQUIRE_THROWS_AS(root.inflation(), ex_INFLATION_NOT_TIME);

            REQUIRE(getInflationSeq() == 0);

            closeLedgerOn(*app, 3, 1, 7, 2014);

            auto txFrame = root.tx({inflation()});
            closeLedgerOn(*app, 4, 7, 7, 2014, {txFrame});
            REQUIRE(getInflationSeq() == 1);

            REQUIRE_THROWS_AS(root.inflation(), ex_INFLATION_NOT_TIME);
            REQUIRE(getInflationSeq() == 1);

            closeLedgerOn(*app, 5, 8, 7, 2014);
            root.inflation();
            REQUIRE(getInflationSeq() == 2);

            closeLedgerOn(*app, 6, 14, 7, 2014);
            REQUIRE_THROWS_AS(root.inflation(), ex_INFLATION_NOT_TIME);
            REQUIRE(getInflationSeq() == 2);

            closeLedgerOn(*app, 7, 15, 7, 2014);
            root.inflation();
            REQUIRE(getInflationSeq() == 3);

            closeLedgerOn(*app, 8, 21, 7, 2014);
            REQUIRE_THROWS_AS(root.inflation(), ex_INFLATION_NOT_TIME);
            REQUIRE(getInflationSeq() == 3);

            {
                auto txFrameInflationTwice = root.tx({inflation(), inflation()});
                closeLedgerOn(*app, 9, 28, 7, 2014);
                applyCheck(txFrameInflationTwice, *app);
                auto const& opResults = txFrameInflationTwice->getResult().result.results();
                REQUIRE(opResults[0].tr().inflationResult().code() == INFLATION_SUCCESS);
                REQUIRE(opResults[1].tr().inflationResult().code() == INFLATION_NOT_TIME);
                REQUIRE(getInflationSeq() == 3);
            }

            {
                auto txFrameInflationTwice = root.tx({inflation(), inflation()});
                closeLedgerOn(*app, 10, 29, 7, 2014);
                applyCheck(txFrameInflationTwice, *app);
                auto const& opResults = txFrameInflationTwice->getResult().result.results();
                REQUIRE(opResults[0].tr().inflationResult().code() == INFLATION_SUCCESS);
                REQUIRE(opResults[1].tr().inflationResult().code() == INFLATION_SUCCESS);
                REQUIRE(getInflationSeq() == 5);
            }
        });
    }

    SECTION("total coins")
    {
        REQUIRE(getFeePool() == 0);
        REQUIRE(getTotalCoins() == 1000000000000000000);

        auto voter1 = TestAccount{*app, getAccount("voter1"), 0};
        auto voter2 = TestAccount{*app, getAccount("voter2"), 0};
        auto target1 = TestAccount{*app, getAccount("target1"), 0};
        auto target2 = TestAccount{*app, getAccount("target2"), 0};

        auto minBalance = getCurrentMinBalance(app->getLedgerStateRoot(), 0);
        auto rootBalance = root.getBalance();

        auto voter1tx = root.tx({createAccount(voter1, rootBalance / 6)});
        voter1tx->getEnvelope().tx.fee = 999999999;
        auto voter2tx = root.tx({createAccount(voter2, rootBalance / 3)});
        auto target1tx = root.tx({createAccount(target1, minBalance)});
        auto target2tx = root.tx({createAccount(target2, minBalance)});

        closeLedgerOn(*app, 2, 21, 7, 2014,
                      {voter1tx, voter2tx, target1tx, target2tx});

        REQUIRE(getFeePool() == 1000000299);
        REQUIRE(getTotalCoins() == 1000000000000000000);

        auto t1Public = target1.getPublicKey();
        auto t2Public = target2.getPublicKey();
        auto setInflationDestination1 = voter1.tx({setOptions(
            &t1Public, nullptr, nullptr, nullptr, nullptr, nullptr)});
        auto setInflationDestination2 = voter2.tx({setOptions(
            &t2Public, nullptr, nullptr, nullptr, nullptr, nullptr)});

        closeLedgerOn(*app, 3, 21, 7, 2014,
                      {setInflationDestination1, setInflationDestination2});

        REQUIRE(getFeePool() == 1000000499);
        REQUIRE(getTotalCoins() == 1000000000000000000);

        auto beforeInflationRoot = root.getBalance();
        auto beforeInflationVoter1 = voter1.getBalance();
        auto beforeInflationVoter2 = voter2.getBalance();
        auto beforeInflationTarget1 = target1.getBalance();
        auto beforeInflationTarget2 = target2.getBalance();

        REQUIRE(beforeInflationRoot + beforeInflationVoter1 +
                    beforeInflationVoter2 + beforeInflationTarget1 +
                    beforeInflationTarget2 + getFeePool() ==
                getTotalCoins());

        auto inflationTx = root.tx({inflation()});

        for_versions_to(7, *app, [&] {
            closeLedgerOn(*app, 4, 21, 7, 2014, {inflationTx});

            REQUIRE(getFeePool() == 95361000000301);
            REQUIRE(getTotalCoins() == 1000095361000000298);

            auto afterInflationRoot = root.getBalance();
            auto afterInflationVoter1 = voter1.getBalance();
            auto afterInflationVoter2 = voter2.getBalance();
            auto afterInflationTarget1 = target1.getBalance();
            auto afterInflationTarget2 = target2.getBalance();
            auto inflationError = 95359999999702;

            REQUIRE(beforeInflationRoot == afterInflationRoot + 100);
            REQUIRE(beforeInflationVoter1 == afterInflationVoter1);
            REQUIRE(beforeInflationVoter2 == afterInflationVoter2);
            REQUIRE(beforeInflationTarget1 ==
                    afterInflationTarget1 - 31787000000099);
            REQUIRE(beforeInflationTarget2 ==
                    afterInflationTarget2 - 63574000000199);

            REQUIRE(afterInflationRoot + afterInflationVoter1 +
                        afterInflationVoter2 + afterInflationTarget1 +
                        afterInflationTarget2 + getFeePool() ==
                    getTotalCoins() + inflationError);
        });

        for_versions_from(8, *app, [&] {
            closeLedgerOn(*app, 4, 21, 7, 2014, {inflationTx});

            REQUIRE(getFeePool() == 95361000000301);
            REQUIRE(getTotalCoins() == 1000190721000000000);

            auto afterInflationRoot = root.getBalance();
            auto afterInflationVoter1 = voter1.getBalance();
            auto afterInflationVoter2 = voter2.getBalance();
            auto afterInflationTarget1 = target1.getBalance();
            auto afterInflationTarget2 = target2.getBalance();

            REQUIRE(beforeInflationRoot == afterInflationRoot + 100);
            REQUIRE(beforeInflationVoter1 == afterInflationVoter1);
            REQUIRE(beforeInflationVoter2 == afterInflationVoter2);
            REQUIRE(beforeInflationTarget1 ==
                    afterInflationTarget1 - 31787000000099);
            REQUIRE(beforeInflationTarget2 ==
                    afterInflationTarget2 - 63574000000199);

            REQUIRE(afterInflationRoot + afterInflationVoter1 +
                        afterInflationVoter2 + afterInflationTarget1 +
                        afterInflationTarget2 + getFeePool() ==
                    getTotalCoins());
        });
    }

    // minVote to participate in inflation
    const int64 minVote = 1000000000LL;
    // .05% of all coins
    LedgerState ls(app->getLedgerStateRoot());
    auto header = ls.loadHeader();
    const int64 winnerVote = bigDivide(header->header().totalCoins,
                                       5, 10000, ROUND_DOWN);
    ls.rollback();

    SECTION("inflation scenarios")
    {
        for_all_versions(*app, [&] {
            std::function<int(int)> voteFunc;
            std::function<int64(int)> balanceFunc;
            int nbAccounts = 0;
            int expectedWinners = 0;

            auto verify = [&]() {
                if (nbAccounts != 0)
                {
                    createTestAccounts(*app, nbAccounts, balanceFunc, voteFunc);
                    closeLedgerOn(*app, 2, 21, 7, 2014);

                    doInflation(
                        *app, getCurrentLedgerVersion(app->getLedgerStateRoot()),
                        nbAccounts, balanceFunc, voteFunc, expectedWinners);
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
                balanceFunc = [&](int n) {
                    return 1 + (winnerVote / nbAccounts);
                };
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
        });
    }
}
