// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "scp/SCP.h"
#include "main/Application.h"
#include "main/Config.h"
#include "simulation/Simulation.h"
#include "test/TestAccount.h"

#include "main/test.h"
#include "lib/catch.hpp"
#include "crypto/SHA.h"
#include "test/TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "main/CommandHandler.h"
#include "ledger/LedgerHeaderFrame.h"
#include "simulation/Simulation.h"
#include "overlay/OverlayManager.h"

#include "xdrpp/marshal.h"

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
    auto root = TestAccount::createRoot(*app);
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

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
                networkID, root, a1, root.nextSequenceNumber(), paymentAmount);
            TransactionFramePtr txFrameA2 = createCreateAccountTx(
                networkID, root, b1, root.nextSequenceNumber(), paymentAmount);

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

            while (app->getLedgerManager().getLastClosedLedgerNum() <
                   (app->getHistoryManager().getCheckpointFrequency() + 5))
            {
                app->getClock().crank(true);
            }

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
    auto root = TestAccount::createRoot(*app);

    const int nbAccounts = 2;
    const int nbTransactions = 5;
    SecretKey accounts[nbAccounts];

    const int64_t paymentAmount = app->getLedgerManager().getMinBalance(0);

    int64_t amountPop =
        nbAccounts * nbTransactions * app->getLedgerManager().getTxFee() +
        paymentAmount;

    auto sourceAccount = root.create("source", amountPop);

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
                                          sourceAccount.nextSequenceNumber(), paymentAmount));
            }
            else
            {
                transactions[i].emplace_back(
                    createPaymentTx(networkID, sourceAccount, accounts[i],
                                    sourceAccount.nextSequenceNumber(), paymentAmount));
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
                                           accounts[0], sourceAccount.getLastSequenceNumber() + 5,
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
                                       sourceAccount.nextSequenceNumber(), paymentAmount));
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

    auto& lm = app->getLedgerManager();

    app->getLedgerManager().getCurrentLedgerHeader().maxTxSetSize =
        cfg.DESIRED_MAX_TX_PER_LEDGER;

    // set up world
    auto root = TestAccount::createRoot(*app);

    auto destAccount = root.create("destAccount", 500000000);
    auto accountB = root.create("accountB", 5000000000);
    auto accountC = root.create("accountC", 5000000000);

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    SECTION("over surge")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, root.nextSequenceNumber(),
                                       n + 10));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("over surge random")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, root.nextSequenceNumber(),
                                       n + 10));
        }
        random_shuffle(txSet->mTransactions.begin(),
                       txSet->mTransactions.end());
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("one account paying more")
    {
        // extra transaction would push the account below the reserve
        for (int n = 0; n < 10; n++)
        {
            txSet->add(createPaymentTx(networkID, root, destAccount, root.nextSequenceNumber(),
                                       n + 10));
            auto tx = createPaymentTx(networkID, accountB, destAccount,
                                      accountB.nextSequenceNumber(), n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
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
            auto tx = createPaymentTx(networkID, root, destAccount, root.nextSequenceNumber(),
                                      n + 10);
            tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 2;
            txSet->add(tx);

            tx = createPaymentTx(networkID, accountB, destAccount,
                                 accountB.nextSequenceNumber(), n + 10);
            if (n != 1)
                tx->getEnvelope().tx.fee = tx->getEnvelope().tx.fee * 3;
            txSet->add(tx);
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
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
            txSet->add(createPaymentTx(networkID, root, destAccount, root.nextSequenceNumber(),
                                       n + 10));
            txSet->add(createPaymentTx(networkID, accountB, destAccount,
                                       accountB.nextSequenceNumber(), n + 10));
            txSet->add(createPaymentTx(networkID, accountC, destAccount,
                                       accountC.nextSequenceNumber(), n + 10));
        }
        txSet->sortForHash();
        txSet->surgePricingFilter(lm);
        REQUIRE(txSet->mTransactions.size() == 5);
        REQUIRE(txSet->checkValid(*app));
    }
}

TEST_CASE("SCP Driver", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.DESIRED_MAX_TX_PER_LEDGER = 5;

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    Hash const& networkID = app->getNetworkID();
    app->start();

    app->getLedgerManager().getCurrentLedgerHeader().maxTxSetSize =
        cfg.DESIRED_MAX_TX_PER_LEDGER;

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto root = TestAccount::createRoot(*app);
    SecretKey a1 = getAccount("A");

    SECTION("combineCandidates")
    {
        auto& herder = *static_cast<HerderImpl*>(&app->getHerder());

        std::set<Value> candidates;

        auto addToCandidates = [&](TxSetFramePtr txSet, uint64_t closeTime)
        {
            txSet->sortForHash();
            herder.recvTxSet(txSet->getContentsHash(), *txSet);

            StellarValue sv(txSet->getContentsHash(), closeTime,
                            emptyUpgradeSteps, 0);
            candidates.emplace(xdr::xdr_to_opaque(sv));
        };
        auto addTransactions = [&](TxSetFramePtr txSet, int n)
        {
            for (int i = 0; i < n; i++)
            {
                txSet->mTransactions.emplace_back(createCreateAccountTx(
                    networkID, root, a1, root.nextSequenceNumber(), 10000000));
            }
        };

        TxSetFramePtr txSet0 = std::make_shared<TxSetFrame>(lcl.hash);
        addToCandidates(txSet0, 100);

        Value v;
        StellarValue sv;

        v = herder.combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.closeTime == 100);
        REQUIRE(sv.txSetHash == txSet0->getContentsHash());

        TxSetFramePtr txSet1 = std::make_shared<TxSetFrame>(lcl.hash);
        addTransactions(txSet1, 10);

        addToCandidates(txSet1, 10);
        v = herder.combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.closeTime == 100);
        REQUIRE(sv.txSetHash == txSet1->getContentsHash());

        TxSetFramePtr txSet2 = std::make_shared<TxSetFrame>(lcl.hash);
        addTransactions(txSet2, 5);

        addToCandidates(txSet2, 1000);
        v = herder.combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.closeTime == 1000);
        REQUIRE(sv.txSetHash == txSet1->getContentsHash());
    }
}

TEST_CASE("SCP State", "[herder]")
{
    SecretKey nodeKeys[3];
    PublicKey nodeIDs[3];

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer sim =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    Config nodeCfgs[3];

    for (int i = 0; i < 3; i++)
    {
        nodeKeys[i] = SecretKey::random();
        nodeIDs[i] = nodeKeys[i].getPublicKey();
        nodeCfgs[i] =
            getTestConfig(i + 1, Config::TestDbMode::TESTDB_ON_DISK_SQLITE);
    }

    VirtualClock* clock = &sim->getClock();

    LedgerHeaderHistoryEntry lcl;

    auto doTest = [&](bool forceSCP)
    {
        // add node0 and node1, in lockstep
        {
            SCPQuorumSet qSet;
            qSet.threshold = 2;
            qSet.validators.push_back(nodeIDs[0]);
            qSet.validators.push_back(nodeIDs[1]);

            sim->addNode(nodeKeys[0], qSet, *clock, &nodeCfgs[0]);
            sim->addNode(nodeKeys[1], qSet, *clock, &nodeCfgs[1]);
            sim->addPendingConnection(nodeIDs[0], nodeIDs[1]);
        }

        sim->startAllNodes();
        // wait to close exactly once

        sim->crankUntil(
            [&]()
            {
                return sim->haveAllExternalized(2, 1);
            },
            std::chrono::seconds(1), true);

        REQUIRE(sim->getNode(nodeIDs[0])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == 2);
        REQUIRE(sim->getNode(nodeIDs[1])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == 2);

        lcl = sim->getNode(nodeIDs[0])
                  ->getLedgerManager()
                  .getLastClosedLedgerHeader();

        // adjust configs for a clean restart
        for (int i = 0; i < 2; i++)
        {
            nodeCfgs[i] = sim->getNode(nodeIDs[i])->getConfig();
            nodeCfgs[i].FORCE_SCP = forceSCP;
        }

        // restart simulation
        sim.reset();

        sim =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);
        clock = &sim->getClock();

        // start a new node that will switch to whatever node0 & node1 says
        SCPQuorumSet qSetAll;
        qSetAll.threshold = 2;
        for (int i = 0; i < 3; i++)
        {
            qSetAll.validators.push_back(nodeIDs[i]);
        }
        sim->addNode(nodeKeys[2], qSetAll, *clock, &nodeCfgs[2]);
        sim->getNode(nodeIDs[2])->start();

        // crank a bit (nothing should happen, node 2 is waiting for SCP
        // messages)
        sim->crankForAtLeast(std::chrono::seconds(1), false);

        REQUIRE(sim->getNode(nodeIDs[2])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == 1);

        // start up node 0 and 1 again
        // nodes 0 and 1 have lost their SCP state as they got restarted
        // yet they should have their own last statements that should be
        // forwarded to node 2 when they connect to it
        // causing node 2 to externalize ledger #2

        sim->addNode(nodeKeys[0], qSetAll, *clock, &nodeCfgs[0], false);
        sim->addNode(nodeKeys[1], qSetAll, *clock, &nodeCfgs[1], false);
        sim->getNode(nodeIDs[0])->start();
        sim->getNode(nodeIDs[1])->start();

        sim->addConnection(nodeIDs[0], nodeIDs[2]);
        sim->addConnection(nodeIDs[1], nodeIDs[2]);
    };

    SECTION("Force SCP")
    {
        doTest(true);

        // then let the nodes run a bit more, they should all externalize the
        // next ledger
        sim->crankUntil(
            [&]()
            {
                return sim->haveAllExternalized(3, 2);
            },
            Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        // nodes are at least on ledger 3 (some may be on 4)
        for (int i = 0; i <= 2; i++)
        {
            auto const& actual = sim->getNode(nodeIDs[i])
                                     ->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header;
            if (actual.ledgerSeq == 3)
            {
                REQUIRE(actual.previousLedgerHash == lcl.hash);
            }
        }
    }

    SECTION("No Force SCP")
    {
        // node 0 and 1 don't try to close, causing all nodes
        // to get stuck at ledger #2
        doTest(false);

        sim->crankUntil(
            [&]()
            {
                return sim->getNode(nodeIDs[2])
                           ->getLedgerManager()
                           .getLastClosedLedgerNum() == 2;
            },
            std::chrono::seconds(1), false);

        for (int i = 0; i <= 2; i++)
        {
            auto const& actual = sim->getNode(nodeIDs[i])
                                     ->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header;
            REQUIRE(actual == lcl.header);
        }
    }
}
