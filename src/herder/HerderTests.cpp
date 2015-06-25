// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "scp/SCP.h"
#include "main/Application.h"
#include "main/Config.h"
#include "simulation/Simulation.h"

#include "main/test.h"
#include "lib/catch.hpp"
#include "crypto/SHA.h"
#include "transactions/TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("standalone", "[herder]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());

    cfg.VALIDATION_KEY = v0SecretKey;

    cfg.QUORUM_SET.threshold = 1;
    cfg.QUORUM_SET.validators.clear();
    cfg.QUORUM_SET.validators.push_back(v0NodeID);

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    app->start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

    AccountFrame::pointer rootAccount = loadAccount(root, *app);

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;
    SECTION("basic ledger close on valid txs")
    {
        bool stop = false;
        VirtualTimer setupTimer(*app);
        VirtualTimer checkTimer(*app);

        auto check = [&](asio::error_code const& error)
        {
            stop = true;

            REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() > 2);

            AccountFrame::pointer a1Account, b1Account;
            a1Account = loadAccount(a1, *app);
            b1Account = loadAccount(b1, *app);
            REQUIRE(a1Account->getBalance() == paymentAmount);
            REQUIRE(b1Account->getBalance() == paymentAmount);
        };

        auto setup = [&](asio::error_code const& error)
        {
            // create accounts
            TransactionFramePtr txFrameA1 =
                createCreateAccountTx(root, a1, rootSeq++, paymentAmount);
            TransactionFramePtr txFrameA2 =
                createCreateAccountTx(root, b1, rootSeq++, paymentAmount);

            REQUIRE(app->getHerder().recvTransaction(txFrameA1) ==
                    Herder::TX_STATUS_PENDING);
            REQUIRE(app->getHerder().recvTransaction(txFrameA2) ==
                    Herder::TX_STATUS_PENDING);
        };

        setupTimer.expires_from_now(std::chrono::seconds(0));
        setupTimer.async_wait(setup);

        checkTimer.expires_from_now(Herder::EXP_LEDGER_TIMESPAN_SECONDS +
                                    std::chrono::seconds(1));
        checkTimer.async_wait(check);

        while (!stop)
        {
            app->getClock().crank(true);
        }
    }
}

// see if we flood at the right times
//  invalid tx
//  normal tx
//  tx with bad seq num
//  account can't pay for all the tx
//  account has just enough for all the tx
//  tx from account not in the DB
TEST_CASE("recvTx", "[herder]")
{
}

TEST_CASE("txset", "[herder]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    app->start();

    // set up world
    SecretKey root = getRoot();

    const int nbAccounts = 2;
    const int nbTransactions = 5;
    SecretKey accounts[nbAccounts];

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

    AccountFrame::pointer rootAccount;

    rootAccount = loadAccount(root, *app);

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    SecretKey sourceAccount = getAccount("source");

    int64_t amountPop =
        nbAccounts * nbTransactions * app->getLedgerManager().getTxFee() +
        paymentAmount;

    applyCreateAccountTx(*app, root, sourceAccount, rootSeq++, amountPop);

    SequenceNumber sourceSeq = getAccountSeqNum(sourceAccount, *app) + 1;

    std::vector<std::vector<TransactionFramePtr>> transactions;

    for (int i = 0; i < nbAccounts; i++)
    {
        std::string accountName = "A";
        accountName += '0' + (char)i;
        accounts[i] = getAccount(accountName.c_str());
        transactions.push_back(std::vector<TransactionFramePtr>());
        for (int j = 0; j < nbTransactions; j++)
        {
            if (j == 0)
            {
                transactions[i].emplace_back(createCreateAccountTx(
                    sourceAccount, accounts[i], sourceSeq++, paymentAmount));
            }
            else
            {
                transactions[i].emplace_back(createPaymentTx(
                    sourceAccount, accounts[i], sourceSeq++, paymentAmount));
            }
        }
    }

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    for (auto& txs : transactions)
    {
        for (auto& tx : txs)
        {
            txSet->add(tx);
        }
    }

    SECTION("order check")
    {
        txSet->sortForHash();

        SECTION("success")
        {
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("out of order")
        {
            std::swap(txSet->mTransactions[0], txSet->mTransactions[1]);
            REQUIRE(!txSet->checkValid(*app));
        }
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            txSet->add(createPaymentTx(accounts[0], root, 1, paymentAmount));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));
        }
        SECTION("sequence gap")
        {
            SECTION("gap after")
            {
                txSet->add(createPaymentTx(sourceAccount, accounts[0],
                                           sourceSeq + 5, paymentAmount));
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));
            }
            SECTION("gap begin")
            {
                txSet->mTransactions.erase(txSet->mTransactions.begin());
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));
            }
            SECTION("gap middle")
            {
                txSet->mTransactions.erase(txSet->mTransactions.begin() + 3);
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));
            }
        }
        SECTION("insuficient balance")
        {
            // extra transaction would push the account below the reserve
            txSet->add(createPaymentTx(sourceAccount, accounts[0], sourceSeq++,
                                       paymentAmount));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));
        }
    }
}

// under surge
// over surge
// make sure it drops the correct txs
// txs with high fee but low ratio
TEST_CASE("surge", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.DESIRED_MAX_TX_PER_LEDGER = 5;

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    app->start();

    // set up world
    SecretKey root = getRoot();

    AccountFrame::pointer rootAccount;

    SecretKey destAccount = getAccount("destAccount");

    rootAccount = loadAccount(root, *app);

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    applyCreateAccountTx(*app, root, destAccount, rootSeq++, 50000000);

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    SECTION("over surge")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(root, destAccount, rootSeq++, n));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
    }

    SECTION("high fee low ratio")
    {
    }
}
