// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "scp/SCP.h"
#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "main/Config.h"
#include "simulation/Simulation.h"

#include <cassert>
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "transactions/TxTests.h"
#include "database/Database.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("standalone", "[herder]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());

    cfg.VALIDATION_KEY = v0SecretKey;

    cfg.QUORUM_THRESHOLD = 1;
    cfg.QUORUM_SET.push_back(v0NodeID);

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    app->start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

    AccountFrame rootAccount;
    REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount,
                                      app->getDatabase()));

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;
    SECTION("basic ledger close on valid txs")
    {
        bool stop = false;
        VirtualTimer setupTimer(*app);
        VirtualTimer checkTimer(*app);

        auto check = [&](asio::error_code const& error)
        {
            stop = true;

            AccountFrame a1Account, b1Account;
            REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account,
                                              app->getDatabase()));
            REQUIRE(AccountFrame::loadAccount(b1.getPublicKey(), b1Account,
                                              app->getDatabase()));

            REQUIRE(a1Account.getBalance() == paymentAmount);
            REQUIRE(b1Account.getBalance() == paymentAmount);
        };

        auto setup = [&](asio::error_code const& error)
        {
            // create accounts
            TransactionFramePtr txFrameA1 =
                createPaymentTx(root, a1, rootSeq++, paymentAmount);
            TransactionFramePtr txFrameA2 =
                createPaymentTx(root, b1, rootSeq++, paymentAmount);

            REQUIRE(app->getHerder().recvTransaction(txFrameA1));
            REQUIRE(app->getHerder().recvTransaction(txFrameA2));
        };

        setupTimer.expires_from_now(std::chrono::seconds(0));
        setupTimer.async_wait(setup);

        checkTimer.expires_from_now(std::chrono::seconds(5));
        checkTimer.async_wait(check);

        while (!stop && app->getClock().crank(false) > 0)
            ;
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

    AccountFrame rootAccount;

    REQUIRE(AccountFrame::loadAccount(root.getPublicKey(), rootAccount,
                                      app->getDatabase()));

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    SecretKey sourceAccount = getAccount("source");

    int64_t amountPop =
        nbAccounts * nbTransactions * app->getLedgerManager().getTxFee() +
        paymentAmount;

    applyPaymentTx(*app, root, sourceAccount, rootSeq++, amountPop);

    SequenceNumber sourceSeq = getAccountSeqNum(sourceAccount, *app) + 1;

    std::vector<TransactionFramePtr> transactions[nbAccounts];

    for (char i = 0; i < nbAccounts; i++)
    {
        std::string accountName = "A";
        accountName += '0' + i;
        accounts[i] = getAccount(accountName.c_str());
        for (int j = 0; j < nbTransactions; j++)
        {
            transactions[i].emplace_back(createPaymentTx(
                sourceAccount, accounts[i], sourceSeq++, paymentAmount));
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
