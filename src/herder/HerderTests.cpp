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
#include "main/CommandHandler.h"
#include "ledger/LedgerHeaderFrame.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("standalone", "[herder]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());

    cfg.NODE_SEED = v0SecretKey;

    cfg.QUORUM_SET.threshold = 1;
    cfg.QUORUM_SET.validators.clear();
    cfg.QUORUM_SET.validators.push_back(v0NodeID);

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();

    app->start();

    // set up world
    SecretKey root = getRoot(networkID);
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
            TransactionFramePtr txFrameA1 = createCreateAccountTx(
                networkID, root, a1, rootSeq++, paymentAmount);
            TransactionFramePtr txFrameA2 = createCreateAccountTx(
                networkID, root, b1, rootSeq++, paymentAmount);

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

        SECTION("Queue processing test")
        {
            app->getCommandHandler().manualCmd("maintenance?queue=true");

            app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=1");
            app->getCommandHandler().manualCmd("maintenance?queue=true");
            auto& db = app->getDatabase();
            auto& sess = db.getSession();
            LedgerHeaderFrame::pointer lh;

            app->getCommandHandler().manualCmd("setcursor?id=A2&cursor=3");
            app->getCommandHandler().manualCmd("maintenance?queue=true");
            lh = LedgerHeaderFrame::loadBySequence(2, db, sess);
            REQUIRE(!!lh);

            app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=2");
            // this should delete items older than sequence 2
            app->getCommandHandler().manualCmd("maintenance?queue=true");
            lh = LedgerHeaderFrame::loadBySequence(2, db, sess);
            REQUIRE(!lh);
            lh = LedgerHeaderFrame::loadBySequence(3, db, sess);
            REQUIRE(!!lh);

            // this should delete items older than sequence 3
            SECTION("set min to 3 by update")
            {
                app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=3");
                app->getCommandHandler().manualCmd("maintenance?queue=true");
                lh = LedgerHeaderFrame::loadBySequence(3, db, sess);
                REQUIRE(!lh);
            }
            SECTION("set min to 3 by deletion")
            {
                app->getCommandHandler().manualCmd("dropcursor?id=A1");
                app->getCommandHandler().manualCmd("maintenance?queue=true");
                lh = LedgerHeaderFrame::loadBySequence(3, db, sess);
                REQUIRE(!lh);
            }
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

    Hash const& networkID = app->getNetworkID();

    app->start();

    // set up world
    SecretKey root = getRoot(networkID);

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
                transactions[i].emplace_back(
                    createCreateAccountTx(networkID, sourceAccount, accounts[i],
                                          sourceSeq++, paymentAmount));
            }
            else
            {
                transactions[i].emplace_back(
                    createPaymentTx(networkID, sourceAccount, accounts[i],
                                    sourceSeq++, paymentAmount));
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

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("out of order")
        {
            std::swap(txSet->mTransactions[0], txSet->mTransactions[1]);
            REQUIRE(!txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            txSet->add(createPaymentTx(networkID, accounts[0], root, 1,
                                       paymentAmount));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("sequence gap")
        {
            SECTION("gap after")
            {
                txSet->add(createPaymentTx(networkID, sourceAccount,
                                           accounts[0], sourceSeq + 5,
                                           paymentAmount));
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                std::vector<TransactionFramePtr> removed;
                txSet->trimInvalid(*app, removed);
                REQUIRE(txSet->checkValid(*app));
            }
            SECTION("gap begin")
            {
                txSet->mTransactions.erase(txSet->mTransactions.begin());
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                std::vector<TransactionFramePtr> removed;
                txSet->trimInvalid(*app, removed);
                REQUIRE(txSet->checkValid(*app));
            }
            SECTION("gap middle")
            {
                txSet->mTransactions.erase(txSet->mTransactions.begin() + 3);
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                std::vector<TransactionFramePtr> removed;
                txSet->trimInvalid(*app, removed);
                REQUIRE(txSet->checkValid(*app));
            }
        }
        SECTION("insuficient balance")
        {
            // extra transaction would push the account below the reserve
            txSet->add(createPaymentTx(networkID, sourceAccount, accounts[0],
                                       sourceSeq++, paymentAmount));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            std::vector<TransactionFramePtr> removed;
            txSet->trimInvalid(*app, removed);
            REQUIRE(txSet->checkValid(*app));
        }
    }
}

// under surge
// over surge
// make sure it drops the correct txs
// txs with high fee but low ratio
// txs from same account high ratio with high seq
TEST_CASE("surge", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.DESIRED_MAX_TX_PER_LEDGER = 5;

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();

    app->start();

    // set up world
    SecretKey root = getRoot(networkID);

    AccountFrame::pointer rootAccount;

    SecretKey destAccount = getAccount("destAccount");

    rootAccount = loadAccount(root, *app);

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    applyCreateAccountTx(*app, root, destAccount, rootSeq++, 50000000);

    SecretKey accountB = getAccount("accountB");
    applyCreateAccountTx(*app, root, accountB, rootSeq++, 500000000);
    SequenceNumber accountBSeq = getAccountSeqNum(accountB, *app) + 1;

    SecretKey accountC = getAccount("accountC");
    applyCreateAccountTx(*app, root, accountC, rootSeq++, 500000000);
    SequenceNumber accountCSeq = getAccountSeqNum(accountC, *app) + 1;

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    SECTION("over surge")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, rootSeq++,
                                       n + 10));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("over surge random")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, rootSeq++,
                                       n + 10));
        }
        random_shuffle(txSet->mTransactions.begin(),
                       txSet->mTransactions.end());
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("one account paying more")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, rootSeq++,
                                       n + 10));
            auto tx = createPaymentTx(networkID, accountB, destAccount,
                                      accountBSeq++, n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
        for (auto& tx : txSet->mTransactions)
        {
            REQUIRE(tx->getSourceID() == accountB.getPublicKey());
        }
    }
    SECTION("one account paying more except for one tx")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            auto tx = createPaymentTx(networkID, root, destAccount, rootSeq++,
                                      n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);

            tx = createPaymentTx(networkID, accountB, destAccount,
                                 accountBSeq++, n + 10);
            if (n != 1)
                tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 3;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
        for (auto& tx : txSet->mTransactions)
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("one account paying more except for one tx")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            auto tx = createPaymentTx(networkID, root, destAccount, rootSeq++,
                                      n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);

            tx = createPaymentTx(networkID, accountB, destAccount,
                                 accountBSeq++, n + 10);
            if (n != 1)
                tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 3;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
        for (auto& tx : txSet->mTransactions)
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("a lot of txs")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 30; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, rootSeq++,
                                       n + 10));
            txSet->add(createPaymentTx(networkID, accountB, destAccount,
                                       accountBSeq++, n + 10));
            txSet->add(createPaymentTx(networkID, accountC, destAccount,
                                       accountCSeq++, n + 10));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }
}
