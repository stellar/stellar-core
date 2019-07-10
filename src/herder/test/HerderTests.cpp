// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/SCP.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/test.h"

#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/CommandHandler.h"
#include "overlay/OverlayManager.h"
#include "test/TxTests.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionFrame.h"

#include "xdrpp/marshal.h"
#include <algorithm>

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("standalone", "[herder][acceptance]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());

    cfg.NODE_SEED = v0SecretKey;

    cfg.QUORUM_SET.threshold = 1;
    cfg.QUORUM_SET.validators.clear();
    cfg.QUORUM_SET.validators.push_back(v0NodeID);

    for_all_versions(cfg, [&](Config const& cfg1) {

        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg1);

        app->start();

        // set up world
        auto root = TestAccount::createRoot(*app);
        auto a1 = TestAccount{*app, getAccount("A")};
        auto b1 = TestAccount{*app, getAccount("B")};
        auto c1 = TestAccount{*app, getAccount("C")};

        auto txfee = app->getLedgerManager().getLastTxFee();
        const int64_t minBalance = app->getLedgerManager().getLastMinBalance(0);
        const int64_t paymentAmount = 100;
        const int64_t startingBalance =
            minBalance + (paymentAmount + txfee) * 3;

        SECTION("basic ledger close on valid txs")
        {
            VirtualTimer setupTimer(*app);

            auto feedTx = [&](TransactionFramePtr& tx) {
                REQUIRE(app->getHerder().recvTransaction(tx) ==
                        TransactionQueue::AddResult::ADD_STATUS_PENDING);
            };

            auto waitForExternalize = [&]() {
                bool stop = false;
                auto prev = app->getLedgerManager().getLastClosedLedgerNum();
                VirtualTimer checkTimer(*app);

                auto check = [&](asio::error_code const& error) {
                    REQUIRE(!error);
                    REQUIRE(app->getLedgerManager().getLastClosedLedgerNum() >
                            prev);
                    stop = true;
                };

                checkTimer.expires_from_now(
                    Herder::EXP_LEDGER_TIMESPAN_SECONDS +
                    std::chrono::seconds(1));
                checkTimer.async_wait(check);
                while (!stop)
                {
                    app->getClock().crank(true);
                }
            };

            auto setup = [&](asio::error_code const& error) {
                REQUIRE(!error);
                // create accounts
                auto txFrameA = root.tx({createAccount(a1, startingBalance)});
                auto txFrameB = root.tx({createAccount(b1, startingBalance)});
                auto txFrameC = root.tx({createAccount(c1, startingBalance)});

                feedTx(txFrameA);
                feedTx(txFrameB);
                feedTx(txFrameC);
            };

            setupTimer.expires_from_now(std::chrono::seconds(0));
            setupTimer.async_wait(setup);

            waitForExternalize();
            auto a1OldSeqNum = a1.getLastSequenceNumber();

            REQUIRE(a1.getBalance() == startingBalance);
            REQUIRE(b1.getBalance() == startingBalance);
            REQUIRE(c1.getBalance() == startingBalance);

            SECTION("txset with valid txs - but failing later")
            {
                std::vector<TransactionFramePtr> txAs, txBs, txCs;
                txAs.emplace_back(a1.tx({payment(root, paymentAmount)}));
                txAs.emplace_back(a1.tx({payment(root, paymentAmount)}));
                txAs.emplace_back(a1.tx({payment(root, paymentAmount)}));

                txBs.emplace_back(b1.tx({payment(root, paymentAmount)}));
                txBs.emplace_back(b1.tx({accountMerge(root)}));
                txBs.emplace_back(b1.tx({payment(a1, paymentAmount)}));

                auto expectedC1Seq = c1.getLastSequenceNumber() + 10;
                txCs.emplace_back(c1.tx({payment(root, paymentAmount)}));
                txCs.emplace_back(c1.tx({bumpSequence(expectedC1Seq)}));
                txCs.emplace_back(c1.tx({payment(root, paymentAmount)}));

                for (auto a : txAs)
                {
                    feedTx(a);
                }
                for (auto b : txBs)
                {
                    feedTx(b);
                }

                bool hasC = false;
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    hasC = ltx.loadHeader().current().ledgerVersion >= 10;
                }
                if (hasC)
                {
                    for (auto c : txCs)
                    {
                        feedTx(c);
                    }
                }

                waitForExternalize();

                // all of a1's transactions went through
                // b1's last transaction failed due to account non existant
                int64 expectedBalance =
                    startingBalance - 3 * paymentAmount - 3 * txfee;
                REQUIRE(a1.getBalance() == expectedBalance);
                REQUIRE(a1.loadSequenceNumber() == a1OldSeqNum + 3);
                REQUIRE(!b1.exists());

                if (hasC)
                {
                    // c1's last transaction failed due to wrong sequence number
                    int64 expectedCBalance =
                        startingBalance - paymentAmount - 3 * txfee;
                    REQUIRE(c1.getBalance() == expectedCBalance);
                    REQUIRE(c1.loadSequenceNumber() == expectedC1Seq);
                }
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

                app->getCommandHandler().manualCmd("setcursor?id=A2&cursor=3");
                app->getCommandHandler().manualCmd("maintenance?queue=true");
                auto lh = LedgerHeaderUtils::loadBySequence(db, sess, 2);
                REQUIRE(!!lh);

                app->getCommandHandler().manualCmd("setcursor?id=A1&cursor=2");
                // this should delete items older than sequence 2
                app->getCommandHandler().manualCmd("maintenance?queue=true");
                lh = LedgerHeaderUtils::loadBySequence(db, sess, 2);
                REQUIRE(!lh);
                lh = LedgerHeaderUtils::loadBySequence(db, sess, 3);
                REQUIRE(!!lh);

                // this should delete items older than sequence 3
                SECTION("set min to 3 by update")
                {
                    app->getCommandHandler().manualCmd(
                        "setcursor?id=A1&cursor=3");
                    app->getCommandHandler().manualCmd(
                        "maintenance?queue=true");
                    lh = LedgerHeaderUtils::loadBySequence(db, sess, 3);
                    REQUIRE(!lh);
                }
                SECTION("set min to 3 by deletion")
                {
                    app->getCommandHandler().manualCmd("dropcursor?id=A1");
                    app->getCommandHandler().manualCmd(
                        "maintenance?queue=true");
                    lh = LedgerHeaderUtils::loadBySequence(db, sess, 3);
                    REQUIRE(!lh);
                }
            }
        }
    });
}

static TransactionFramePtr
makeMultiPayment(stellar::TestAccount& destAccount, stellar::TestAccount& src,
                 int nbOps, int64 paymentBase, uint32 extraFee, uint32 feeMult)
{
    std::vector<stellar::Operation> ops;
    for (int i = 0; i < nbOps; i++)
    {
        ops.emplace_back(payment(destAccount, i + paymentBase));
    }
    auto tx = src.tx(ops);
    tx->getEnvelope().tx.fee *= feeMult;
    tx->getEnvelope().tx.fee += extraFee;
    tx->getEnvelope().signatures.clear();
    tx->addSignature(src);
    return tx;
}

static void
testTxSet(uint32 protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 14;
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    app->start();

    LedgerHeader lhCopy;
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        lhCopy = ltx.loadHeader().current();
    }

    // set up world
    auto root = TestAccount::createRoot(*app);

    const int nbAccounts = 2;
    const int nbTransactions = 5;

    auto accounts = std::vector<TestAccount>{};

    const int64_t minBalance0 = app->getLedgerManager().getLastMinBalance(0);

    // amount only allows up to nbTransactions
    int64_t amountPop =
        nbTransactions * app->getLedgerManager().getLastTxFee() + minBalance0;

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    auto genTx = [&](int nbTxs) {
        std::string accountName = fmt::format("A{}", accounts.size());
        accounts.push_back(root.create(accountName.c_str(), amountPop));
        auto& account = accounts.back();
        for (int j = 0; j < nbTxs; j++)
        {
            // payment to self
            txSet->add(account.tx({payment(account.getPublicKey(), 10000)}));
        }
    };

    for (size_t i = 0; i < nbAccounts; i++)
    {
        genTx(nbTransactions);
    }

    SECTION("too many txs")
    {
        while (txSet->mTransactions.size() <=
               cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE)
        {
            genTx(1);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
    }
    SECTION("order check")
    {
        txSet->sortForHash();

        SECTION("success")
        {
            REQUIRE(txSet->checkValid(*app));

            txSet->trimInvalid(*app);
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("out of order")
        {
            std::swap(txSet->mTransactions[0], txSet->mTransactions[1]);
            REQUIRE(!txSet->checkValid(*app));

            txSet->trimInvalid(*app);
            REQUIRE(txSet->checkValid(*app));
        }
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            auto newUser = TestAccount{*app, getAccount("doesnotexist")};
            txSet->add(newUser.tx({payment(root, 1)}));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            txSet->trimInvalid(*app);
            REQUIRE(txSet->checkValid(*app));
        }
        SECTION("sequence gap")
        {
            SECTION("gap after")
            {
                auto tx = accounts[0].tx({payment(accounts[0], 1)});
                tx->getEnvelope().tx.seqNum += 5;
                txSet->add(tx);
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                txSet->trimInvalid(*app);
                REQUIRE(txSet->checkValid(*app));
            }
            SECTION("gap begin")
            {
                txSet->sortForApply();
                txSet->mTransactions.erase(txSet->mTransactions.begin());
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                auto removed = txSet->trimInvalid(*app);
                REQUIRE(txSet->checkValid(*app));
                // one of the account lost all its transactions
                REQUIRE(removed.size() == (nbTransactions - 1));
                REQUIRE(txSet->mTransactions.size() == nbTransactions);
            }
            SECTION("gap middle")
            {
                int remIdx = 2; // 3rd transaction
                txSet->sortForApply();
                txSet->mTransactions.erase(txSet->mTransactions.begin() +
                                           (remIdx * 2));
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app));

                auto removed = txSet->trimInvalid(*app);
                REQUIRE(txSet->checkValid(*app));
                // one account has all its transactions,
                // other, we removed all its tx
                REQUIRE(removed.size() == (nbTransactions - 1));
                REQUIRE(txSet->mTransactions.size() == nbTransactions);
            }
        }
        SECTION("insufficient balance")
        {
            // extra transaction would push the account below the reserve
            txSet->add(accounts[0].tx({payment(accounts[0], 10)}));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            auto removed = txSet->trimInvalid(*app);
            REQUIRE(txSet->checkValid(*app));
            REQUIRE(removed.size() == (nbTransactions + 1));
            REQUIRE(txSet->mTransactions.size() == nbTransactions);
        }
        SECTION("bad signature")
        {
            auto tx = txSet->mTransactions[0];
            tx->getEnvelope().tx.timeBounds.activate().maxTime = UINT64_MAX;
            tx->clearCached();
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));
        }
    }
}

TEST_CASE("txset", "[herder][txset]")
{
    SECTION("protocol 10")
    {
        testTxSet(10);
    }
    SECTION("protocol current")
    {
        testTxSet(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    }
}

TEST_CASE("txset base fee", "[herder][txset]")
{
    Config cfg(getTestConfig());
    uint32_t const maxTxSize = 112;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSize;

    auto testBaseFee = [&](uint32_t protocolVersion, uint32 nbTransactions,
                           uint32 extraAccounts, size_t lim, int64_t expLowFee,
                           int64_t expHighFee) {
        cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        app->start();

        LedgerHeader lhCopy;
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            lhCopy = ltx.loadHeader().current();
        }

        // set up world
        auto root = TestAccount::createRoot(*app);

        int64 startingBalance =
            app->getLedgerManager().getLastMinBalance(0) + 10000000;

        auto accounts = std::vector<TestAccount>{};

        TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        for (uint32 i = 0; i < nbTransactions; i++)
        {
            std::string nameI = fmt::format("Base{}", i);
            auto aI = root.create(nameI, startingBalance);
            accounts.push_back(aI);

            auto tx = makeMultiPayment(aI, aI, 1, 1000, 0, 10);
            txSet->add(tx);
        }

        for (uint32 k = 1; k <= extraAccounts; k++)
        {
            std::string nameI = fmt::format("Extra{}", k);
            auto aI = root.create(nameI, startingBalance);
            accounts.push_back(aI);

            auto tx = makeMultiPayment(aI, aI, 2, 1000, k, 100);
            txSet->add(tx);
        }

        REQUIRE(txSet->size(lhCopy) == lim);
        REQUIRE(extraAccounts >= 2);
        txSet->sortForHash();
        REQUIRE(txSet->checkValid(*app));

        // fetch balances
        auto getBalances = [&]() {
            std::vector<int64_t> balances;
            std::transform(accounts.begin(), accounts.end(),
                           std::back_inserter(balances),
                           [](TestAccount& a) { return a.getBalance(); });
            return balances;
        };
        auto balancesBefore = getBalances();

        // apply this
        closeLedgerOn(*app, 2, 1, 1, 2020, txSet->mTransactions);

        auto balancesAfter = getBalances();
        int64_t lowFee = INT64_MAX, highFee = 0;
        for (size_t i = 0; i < balancesAfter.size(); i++)
        {
            auto b = balancesBefore[i];
            auto a = balancesAfter[i];
            auto fee = b - a;
            lowFee = std::min(lowFee, fee);
            highFee = std::max(highFee, fee);
        }

        REQUIRE(lowFee == expLowFee);
        REQUIRE(highFee == expHighFee);
    };

    // 8 base transactions
    //   1 op, fee bid = baseFee*10 = 1000
    // extra tx
    //   2 ops, fee bid = 20000+i
    // to reach 112
    //    protocol 10 adds 104 tx (208 ops)
    //    protocol 11 adds 52 tx (104 ops)

    // v11: surge threshold is 112-100=12 ops
    //     no surge pricing @ 10 (only 1 extra tx)
    //     surge pricing @ 12 (2 extra tx)

    uint32 const baseCount = 8;
    uint32 const v10ExtraTx = 104;
    uint32 const v11ExtraTx = 52;
    uint32 const v10NewCount = 112;
    uint32 const v11NewCount = 56; // 112/2
    SECTION("surged")
    {
        SECTION("mixed")
        {
            SECTION("protocol 10")
            {
                // low = base tx
                // high = last extra tx
                testBaseFee(10, baseCount, v10ExtraTx, maxTxSize, 1000, 20104);
            }
            SECTION("protocol current")
            {
                // low = 10*base tx = baseFee = 1000
                // high = 2*base (surge)
                SECTION("maxed out surged")
                {
                    testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                baseCount, v11ExtraTx, maxTxSize, 1000, 2000);
                }
                SECTION("smallest surged")
                {
                    testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                baseCount + 1, v11ExtraTx - 50,
                                maxTxSize - 100 + 1, 1000, 2000);
                }
            }
        }
        SECTION("newOnly")
        {
            SECTION("protocol 10")
            {
                // low = 20000+1
                // high = 20000+112
                testBaseFee(10, 0, v10NewCount, maxTxSize, 20001, 20112);
            }
            SECTION("protocol current")
            {
                // low = 20000+1 -> baseFee = 20001/2+ = 10001
                // high = 10001*2
                testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                            v11NewCount, maxTxSize, 20001, 20002);
            }
        }
    }
    SECTION("not surged")
    {
        SECTION("mixed")
        {
            SECTION("protocol 10")
            {
                // low = 1000
                // high = 20000+4
                testBaseFee(10, baseCount, 4, baseCount + 4, 1000, 20004);
            }
            SECTION("protocol current")
            {
                // baseFee = minFee = 100
                // high = 2*minFee
                // highest number of ops not surged is max-100
                testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, baseCount,
                            v11ExtraTx - 50, maxTxSize - 100, 100, 200);
            }
        }
        SECTION("newOnly")
        {
            SECTION("protocol 10")
            {
                // low = 20000+1
                // high = 20000+12
                testBaseFee(10, 0, 12, 12, 20001, 20012);
            }
            SECTION("protocol current")
            {
                // low = minFee = 100
                // high = 2*minFee
                // highest number of ops not surged is max-100
                testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                            v11NewCount - 50, maxTxSize - 100, 200, 200);
            }
        }
    }
}

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

static void
surgeTest(uint32 protocolVersion, uint32_t nbTxs, uint32_t maxTxSetSize,
          uint32_t expectedReduced)
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSetSize;
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    app->start();

    LedgerHeader lhCopy;
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        lhCopy = ltx.loadHeader().current();
    }
    // set up world
    auto root = TestAccount::createRoot(*app);

    auto destAccount = root.create("destAccount", 500000000);
    auto accountB = root.create("accountB", 5000000000);
    auto accountC = root.create("accountC", 5000000000);

    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    auto multiPaymentTx =
        std::bind(makeMultiPayment, destAccount, _1, _2, _3, 0, 100);

    auto addRootTxs = [&]() {
        for (uint32_t n = 0; n < 2 * nbTxs; n++)
        {
            txSet->add(multiPaymentTx(root, n + 1, 10000 + 1000 * n));
        }
    };

    auto surgePricing = [&]() {
        txSet->trimInvalid(*app);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
    };

    SECTION("basic single account")
    {
        auto refSeqNum = root.getLastSequenceNumber();
        addRootTxs();
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app));
        // check that the expected tx are there
        auto txs = txSet->sortForApply();
        for (auto& tx : txs)
        {
            refSeqNum++;
            REQUIRE(tx->getSeqNum() == refSeqNum);
        }
    }

    SECTION("one account paying more")
    {
        addRootTxs();
        for (uint32_t n = 0; n < nbTxs; n++)
        {
            auto tx = multiPaymentTx(accountB, n + 1, 10000 + 1000 * n);
            tx->getEnvelope().tx.fee -= 1;
            tx->getEnvelope().signatures.clear();
            tx->addSignature(accountB);
            txSet->add(tx);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app));
        // check that the expected tx are there
        auto& txs = txSet->mTransactions;
        for (auto& tx : txs)
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("one account with more operations but same total fee")
    {
        addRootTxs();
        for (uint32_t n = 0; n < nbTxs; n++)
        {
            auto tx = multiPaymentTx(accountB, n + 2, 10000 + 1000 * n);
            // find corresponding root tx (should have 1 less op)
            auto rTx = txSet->mTransactions[n];
            REQUIRE(rTx->getEnvelope().tx.operations.size() == n + 1);
            REQUIRE(tx->getEnvelope().tx.operations.size() == n + 2);
            // use the same fee
            tx->getEnvelope().tx.fee = rTx->getEnvelope().tx.fee;
            tx->getEnvelope().signatures.clear();
            tx->addSignature(accountB);
            txSet->add(tx);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app));
        // check that the expected tx are there
        auto& txs = txSet->mTransactions;
        for (auto& tx : txs)
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("one account paying more except for one tx in middle")
    {
        auto refSeqNumRoot = root.getLastSequenceNumber();
        addRootTxs();
        auto refSeqNumB = accountB.getLastSequenceNumber();
        for (uint32_t n = 0; n < nbTxs; n++)
        {
            auto tx = multiPaymentTx(accountB, n + 1, 10000 + 1000 * n);
            if (n == 2)
            {
                tx->getEnvelope().tx.fee -= 1;
            }
            else
            {
                tx->getEnvelope().tx.fee += 1;
            }
            tx->getEnvelope().signatures.clear();
            tx->addSignature(accountB);
            txSet->add(tx);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == expectedReduced);
        REQUIRE(txSet->checkValid(*app));
        // check that the expected tx are there
        auto txs = txSet->sortForApply();
        int nbAccountB = 0;
        for (auto& tx : txs)
        {
            if (tx->getSourceID() == accountB.getPublicKey())
            {
                nbAccountB++;
                refSeqNumB++;
                REQUIRE(tx->getSeqNum() == refSeqNumB);
            }
            else
            {
                refSeqNumRoot++;
                REQUIRE(tx->getSeqNum() == refSeqNumRoot);
            }
        }
        REQUIRE(nbAccountB == 2);
    }

    SECTION("a lot of txs")
    {
        for (uint32_t n = 0; n < nbTxs * 10; n++)
        {
            txSet->add(root.tx({payment(destAccount, n + 10)}));
            txSet->add(accountB.tx({payment(destAccount, n + 10)}));
            txSet->add(accountC.tx({payment(destAccount, n + 10)}));
        }
        txSet->sortForHash();
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app));
    }
}

TEST_CASE("surge pricing", "[herder][txset]")
{
    SECTION("protocol 10")
    {
        surgeTest(10, 5, 5, 5);
    }
    SECTION("protocol current")
    {
        // (1+..+4) + (1+2) = 10+3 = 13
        surgeTest(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 5, 15, 13);
    }
}

static void
testSCPDriver(uint32 protocolVersion, uint32_t maxTxSize, size_t expectedOps,
              bool checkHighFee, bool withSCPsignature)
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSize;

    VirtualClock clock;
    auto s = SecretKey::pseudoRandomForTesting();
    cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());

    Application::pointer app = createTestApplication(clock, cfg);

    app->start();

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto root = TestAccount::createRoot(*app);
    auto a1 = TestAccount{*app, getAccount("A")};

    using TxPair = std::pair<Value, TxSetFramePtr>;
    auto makeTxPair = [&](HerderImpl& herder, TxSetFramePtr txSet,
                          uint64_t closeTime, bool sig) {
        txSet->sortForHash();
        auto sv = StellarValue(txSet->getContentsHash(), closeTime,
                               emptyUpgradeSteps, STELLAR_VALUE_BASIC);
        if (sig)
        {
            herder.signStellarValue(root.getSecretKey(), sv);
        }
        auto v = xdr::xdr_to_opaque(sv);

        return TxPair{v, txSet};
    };
    auto makeEnvelope = [&s](HerderImpl& herder, TxPair const& p, Hash qSetHash,
                             uint64_t slotIndex, bool nomination) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotIndex;
        if (nomination)
        {
            envelope.statement.pledges.type(SCP_ST_NOMINATE);
            envelope.statement.pledges.nominate().votes.push_back(p.first);
            envelope.statement.pledges.nominate().quorumSetHash = qSetHash;
        }
        else
        {
            envelope.statement.pledges.type(SCP_ST_PREPARE);
            envelope.statement.pledges.prepare().ballot.value = p.first;
            envelope.statement.pledges.prepare().quorumSetHash = qSetHash;
        }
        envelope.statement.nodeID = s.getPublicKey();
        herder.signEnvelope(s, envelope);
        return envelope;
    };
    auto addTransactions = [&](TxSetFramePtr txSet, int n, int nbOps,
                               uint32 feeMulti) {
        txSet->mTransactions.resize(n);
        std::generate(std::begin(txSet->mTransactions),
                      std::end(txSet->mTransactions), [&]() {
                          return makeMultiPayment(root, root, nbOps, 1000, 0,
                                                  feeMulti);
                      });
    };
    auto makeTransactions = [&](Hash hash, int n, int nbOps, uint32 feeMulti) {
        root.loadSequenceNumber();
        auto result = std::make_shared<TxSetFrame>(hash);
        addTransactions(result, n, nbOps, feeMulti);
        result->sortForHash();
        REQUIRE(result->checkValid(*app));
        return result;
    };

    SECTION("combineCandidates")
    {
        auto& herder = static_cast<HerderImpl&>(app->getHerder());

        std::set<Value> candidates;

        auto addToCandidates = [&](TxPair const& p) {
            candidates.emplace(p.first);
            auto envelope = makeEnvelope(
                herder, p, {}, herder.getCurrentLedgerSeq() + 1, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p.second->getContentsHash(), *p.second));
        };

        TxSetFramePtr txSet0 = makeTransactions(lcl.hash, 0, 1, 100);
        addToCandidates(makeTxPair(herder, txSet0, 10, withSCPsignature));

        Value v;
        StellarValue sv;

        v = herder.getHerderSCPDriver().combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.ext.v() == STELLAR_VALUE_BASIC);
        REQUIRE(sv.closeTime == 10);
        REQUIRE(sv.txSetHash == txSet0->getContentsHash());

        TxSetFramePtr txSet1 = makeTransactions(lcl.hash, 10, 1, 100);

        addToCandidates(makeTxPair(herder, txSet1, 5, withSCPsignature));
        v = herder.getHerderSCPDriver().combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.ext.v() == STELLAR_VALUE_BASIC);
        REQUIRE(sv.closeTime == 10);
        REQUIRE(sv.txSetHash == txSet1->getContentsHash());

        TxSetFramePtr txSet2 = makeTransactions(lcl.hash, 5, 3, 100);
        addToCandidates(makeTxPair(herder, txSet2, 20, withSCPsignature));

        auto biggestTxSet = txSet1;
        if (biggestTxSet->size(lcl.header) < txSet2->size(lcl.header))
        {
            biggestTxSet = txSet2;
        }

        // picks the biggest set, highest time
        v = herder.getHerderSCPDriver().combineCandidates(1, candidates);
        xdr::xdr_from_opaque(v, sv);
        REQUIRE(sv.ext.v() == STELLAR_VALUE_BASIC);
        REQUIRE(sv.closeTime == 20);
        REQUIRE(sv.txSetHash == biggestTxSet->getContentsHash());
        REQUIRE(biggestTxSet->sizeOp() == expectedOps);

        if (checkHighFee)
        {
            TxSetFramePtr txSetL =
                makeTransactions(lcl.hash, maxTxSize, 1, 101);
            addToCandidates(makeTxPair(herder, txSetL, 20, withSCPsignature));
            TxSetFramePtr txSetL2 =
                makeTransactions(lcl.hash, maxTxSize, 1, 1000);
            addToCandidates(makeTxPair(herder, txSetL2, 20, withSCPsignature));
            v = herder.getHerderSCPDriver().combineCandidates(1, candidates);
            xdr::xdr_from_opaque(v, sv);
            REQUIRE(sv.ext.v() == STELLAR_VALUE_BASIC);
            REQUIRE(sv.txSetHash == txSetL2->getContentsHash());
        }
    }

    SECTION("validateValue signatures")
    {
        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto& scp = herder.getHerderSCPDriver();
        auto seq = herder.getCurrentLedgerSeq() + 1;
        auto ct = app->timeNow() + 1;

        TxSetFramePtr txSet0 = makeTransactions(lcl.hash, 0, 1, 100);
        {
            // make sure that txSet0 is loaded
            auto p = makeTxPair(herder, txSet0, ct, withSCPsignature);
            auto envelope = makeEnvelope(herder, p, {}, seq, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(txSet0->getContentsHash(), *txSet0));
        }

        SECTION("valid")
        {
            auto nomV = makeTxPair(herder, txSet0, ct, withSCPsignature);
            REQUIRE(scp.validateValue(seq, nomV.first, true) ==
                    SCPDriver::kFullyValidatedValue);

            auto balV = makeTxPair(herder, txSet0, ct, false);
            REQUIRE(scp.validateValue(seq, balV.first, false) ==
                    SCPDriver::kFullyValidatedValue);
        }
        SECTION("invalid")
        {
            // nomination, requires signature iff withSCPsignature is true
            auto nomV = makeTxPair(herder, txSet0, ct, !withSCPsignature);
            REQUIRE(scp.validateValue(seq, nomV.first, true) ==
                    SCPDriver::kInvalidValue);

            // ballot protocol, with signature is never valid
            auto balV = makeTxPair(herder, txSet0, ct, true);
            REQUIRE(scp.validateValue(seq, balV.first, false) ==
                    SCPDriver::kInvalidValue);

            if (withSCPsignature)
            {
                auto p = makeTxPair(herder, txSet0, ct, withSCPsignature);
                StellarValue sv;
                xdr::xdr_from_opaque(p.first, sv);

                auto checkInvalid = [&](StellarValue const& sv) {
                    auto v = xdr::xdr_to_opaque(sv);
                    REQUIRE(scp.validateValue(seq, v, true) ==
                            SCPDriver::kInvalidValue);
                };

                // mutate in a few ways
                SECTION("missing signature")
                {
                    sv.ext.lcValueSignature().signature.clear();
                    checkInvalid(sv);
                }
                SECTION("wrong signature")
                {
                    sv.ext.lcValueSignature().signature[0] ^= 1;
                    checkInvalid(sv);
                }
                SECTION("wrong signature 2")
                {
                    sv.ext.lcValueSignature().nodeID.ed25519()[0] ^= 1;
                    checkInvalid(sv);
                }
            }
        }
    }

    SECTION("accept qset and txset")
    {
        auto makePublicKey = [](int i) {
            auto hash = sha256("NODE_SEED_" + std::to_string(i));
            auto secretKey = SecretKey::fromSeed(hash);
            return secretKey.getPublicKey();
        };

        auto makeSingleton = [](const PublicKey& key) {
            auto result = SCPQuorumSet{};
            result.threshold = 1;
            result.validators.push_back(key);
            return result;
        };

        auto keys = std::vector<PublicKey>{};
        for (auto i = 0; i < 1001; i++)
        {
            keys.push_back(makePublicKey(i));
        }

        auto saneQSet1 = makeSingleton(keys[0]);
        auto saneQSet1Hash = sha256(xdr::xdr_to_opaque(saneQSet1));
        auto saneQSet2 = makeSingleton(keys[1]);
        auto saneQSet2Hash = sha256(xdr::xdr_to_opaque(saneQSet2));

        auto bigQSet = SCPQuorumSet{};
        bigQSet.threshold = 1;
        bigQSet.validators.push_back(keys[0]);
        for (auto i = 0; i < 10; i++)
        {
            bigQSet.innerSets.push_back({});
            bigQSet.innerSets.back().threshold = 1;
            for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
                bigQSet.innerSets.back().validators.push_back(keys[j]);
        }
        auto bigQSetHash = sha256(xdr::xdr_to_opaque(bigQSet));

        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto transactions1 = makeTransactions(lcl.hash, 5, 1, 100);
        auto transactions2 = makeTransactions(lcl.hash, 4, 1, 100);
        auto p1 = makeTxPair(herder, transactions1, 10, withSCPsignature);
        auto p2 = makeTxPair(herder, transactions1, 10, withSCPsignature);
        // use current + 1 to allow for any value (old values get filtered more)
        auto lseq = herder.getCurrentLedgerSeq() + 1;
        auto saneEnvelopeQ1T1 =
            makeEnvelope(herder, p1, saneQSet1Hash, lseq, true);
        auto saneEnvelopeQ1T2 =
            makeEnvelope(herder, p2, saneQSet1Hash, lseq, true);
        auto saneEnvelopeQ2T1 =
            makeEnvelope(herder, p1, saneQSet2Hash, lseq, true);
        auto bigEnvelope = makeEnvelope(herder, p1, bigQSetHash, lseq, true);

        SECTION("return FETCHING until fetched")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
            // will not return ENVELOPE_STATUS_READY as the recvSCPEnvelope() is
            // called internally
            // when QSet and TxSet are both received
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);
        }

        SECTION("only accepts qset once")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));

            SECTION("when re-receiving the same envelope")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            }

            SECTION("when receiving different envelope with the same qset")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T2) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            }
        }

        SECTION("only accepts txset once")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));

            SECTION("when re-receiving the same envelope")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(),
                                          *p1.second));
            }

            SECTION("when receiving different envelope with the same txset")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ2T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(),
                                          *p1.second));
            }
        }

        SECTION("do not accept unasked qset")
        {
            REQUIRE(!herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(!herder.recvSCPQuorumSet(saneQSet2Hash, saneQSet2));
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
        }

        SECTION("do not accept unasked txset")
        {
            REQUIRE(
                !herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
            REQUIRE(
                !herder.recvTxSet(p2.second->getContentsHash(), *p2.second));
        }

        SECTION("do not accept not sane qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
        }

        SECTION("do not accept txset from envelope discarded because of unsane "
                "qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
            REQUIRE(
                !herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
        }

        SECTION(
            "accept txset from envelope with unsane qset before receiving qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
        }

        SECTION("accept txset from envelopes with both valid and unsane qset")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(!herder.recvSCPQuorumSet(bigQSetHash, bigQSet));
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), *p1.second));
        }
    }
}

TEST_CASE("SCP Driver", "[herder][acceptance]")
{
    SECTION("protocol 10")
    {
        testSCPDriver(10, 10, 10, false, false);
    }
    SECTION("protocol current")
    {
        testSCPDriver(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 1000, 15, true,
                      true);
    }
}

TEST_CASE("SCP State", "[herder][acceptance]")
{
    SecretKey nodeKeys[3];
    PublicKey nodeIDs[3];

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer sim =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    Config nodeCfgs[3];

    // Normally ledger should externalize in EXP_LEDGER_TIMESPAN_SECONDS
    // but for "Force SCP" test there are 3 nodes and only 2 have previous
    // ledger state. However it is possible that nomination protocol will
    // choose last node as leader for first few rounds. New ledger will only
    // be externalized when first or second node are choosen as round leaders.
    // It some cases it can take more time than expected. Probability of that
    // is pretty low, but high enough that it forced us to rerun tests from
    // time to time to pass that one case.
    //
    // After changing node ids generated here from random to deterministics
    // this problem goes away, as the leader selection protocol uses node id
    // and round id for selecting leader.
    for (int i = 0; i < 3; i++)
    {
        nodeKeys[i] = SecretKey::fromSeed(sha256("Node_" + std::to_string(i)));
        nodeIDs[i] = nodeKeys[i].getPublicKey();
        nodeCfgs[i] =
            getTestConfig(i + 1, Config::TestDbMode::TESTDB_ON_DISK_SQLITE);
    }

    LedgerHeaderHistoryEntry lcl;
    uint32_t numLedgers = 5;
    uint32_t expectedLedger = LedgerManager::GENESIS_LEDGER_SEQ + numLedgers;

    auto doTest = [&](bool forceSCP) {
        // add node0 and node1, in lockstep
        {
            SCPQuorumSet qSet;
            qSet.threshold = 2;
            qSet.validators.push_back(nodeIDs[0]);
            qSet.validators.push_back(nodeIDs[1]);

            sim->addNode(nodeKeys[0], qSet, &nodeCfgs[0]);
            sim->addNode(nodeKeys[1], qSet, &nodeCfgs[1]);
            sim->addPendingConnection(nodeIDs[0], nodeIDs[1]);
        }

        sim->startAllNodes();

        // wait to close a few ledgers
        sim->crankUntil(
            [&]() { return sim->haveAllExternalized(expectedLedger, 1); },
            2 * numLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        REQUIRE(sim->getNode(nodeIDs[0])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == expectedLedger);
        REQUIRE(sim->getNode(nodeIDs[1])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() == expectedLedger);

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

        // start a new node that will switch to whatever node0 & node1 says
        SCPQuorumSet qSetAll;
        qSetAll.threshold = 2;
        for (int i = 0; i < 3; i++)
        {
            qSetAll.validators.push_back(nodeIDs[i]);
        }
        sim->addNode(nodeKeys[2], qSetAll, &nodeCfgs[2]);
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
        // causing node 2 to externalize ledger #6

        sim->addNode(nodeKeys[0], qSetAll, &nodeCfgs[0], false);
        sim->addNode(nodeKeys[1], qSetAll, &nodeCfgs[1], false);
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
            [&]() { return sim->haveAllExternalized(expectedLedger + 1, 5); },
            2 * numLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        // nodes are at least on ledger 7 (some may be on 8)
        for (int i = 0; i <= 2; i++)
        {
            auto const& actual = sim->getNode(nodeIDs[i])
                                     ->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header;
            if (actual.ledgerSeq == expectedLedger + 1)
            {
                REQUIRE(actual.previousLedgerHash == lcl.hash);
            }
        }
    }

    SECTION("No Force SCP")
    {
        // node 0 and 1 don't try to close, causing all nodes
        // to get stuck at ledger #6
        doTest(false);

        sim->crankUntil(
            [&]() {
                return sim->getNode(nodeIDs[2])
                           ->getLedgerManager()
                           .getLastClosedLedgerNum() == expectedLedger;
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

TEST_CASE("quick restart", "[herder][quickRestart]")
{
    auto mode = Simulation::OVER_LOOPBACK;
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = std::make_shared<Simulation>(mode, networkID);

    auto validatorKey = SecretKey::fromSeed(sha256("validator"));
    auto listenerKey = SecretKey::fromSeed(sha256("listener"));

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(validatorKey.getPublicKey());

    simulation->addNode(validatorKey, qSet);
    simulation->addNode(listenerKey, qSet);
    simulation->addPendingConnection(validatorKey.getPublicKey(),
                                     listenerKey.getPublicKey());
    simulation->startAllNodes();

    auto currentValidatorLedger = [&]() {
        auto app = simulation->getNode(validatorKey.getPublicKey());
        return app->getLedgerManager().getLastClosedLedgerNum();
    };
    auto currentListenerLedger = [&]() {
        auto app = simulation->getNode(listenerKey.getPublicKey());
        return app->getLedgerManager().getLastClosedLedgerNum();
    };
    auto waitForLedgersOnValidator = [&](int nLedgers) {
        auto destinationLedger = currentValidatorLedger() + nLedgers;
        simulation->crankUntil(
            [&]() { return currentValidatorLedger() == destinationLedger; },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        return currentValidatorLedger();
    };
    auto waitForLedgers = [&](int nLedgers) {
        auto destinationLedger = currentValidatorLedger() + nLedgers;
        simulation->crankUntil(
            [&]() {
                return simulation->haveAllExternalized(destinationLedger, 100);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        return currentValidatorLedger();
    };

    uint32_t currentLedger = 1;
    REQUIRE(currentValidatorLedger() == currentLedger);
    REQUIRE(currentListenerLedger() == currentLedger);

    auto static const FEW_LEDGERS = 5;

    // externalize a few ledgers
    currentLedger = waitForLedgers(FEW_LEDGERS);

    REQUIRE(currentValidatorLedger() == currentLedger);
    // listener is at most a ledger behind
    REQUIRE((currentLedger - currentListenerLedger()) <= 1);

    // disconnect listener
    simulation->dropConnection(validatorKey.getPublicKey(),
                               listenerKey.getPublicKey());

    // SMALL_GAP happens to be the maximum number of ledgers
    // that are kept in memory
    auto static const SMALL_GAP = Herder::MAX_SLOTS_TO_REMEMBER + 1;
    auto static const BIG_GAP = SMALL_GAP + 1;

    auto beforeGap = currentLedger;

    SECTION("works when gap is small")
    {
        // externalize a few more ledgers
        currentLedger = waitForLedgersOnValidator(SMALL_GAP);

        REQUIRE(currentValidatorLedger() == currentLedger);
        // listener may have processed messages it got before getting
        // disconnected
        REQUIRE(currentListenerLedger() <= beforeGap);

        // and reconnect
        simulation->addConnection(validatorKey.getPublicKey(),
                                  listenerKey.getPublicKey());

        // now listener should catchup to validator without remote history
        currentLedger = waitForLedgers(FEW_LEDGERS);

        REQUIRE(currentValidatorLedger() == currentLedger);
        REQUIRE((currentLedger - currentListenerLedger()) <= 1);
    }

    SECTION("does not work when gap is big")
    {
        // externalize a few more ledgers
        currentLedger = waitForLedgersOnValidator(BIG_GAP);

        REQUIRE(currentValidatorLedger() == currentLedger);
        // listener may have processed messages it got before getting
        // disconnected
        REQUIRE(currentListenerLedger() <= beforeGap);

        // and reconnect
        simulation->addConnection(validatorKey.getPublicKey(),
                                  listenerKey.getPublicKey());

        // wait for few ledgers - listener will want to catchup with history,
        // but will get an exception:
        // "No GET-enabled history archive in config"
        REQUIRE_THROWS_AS(waitForLedgers(FEW_LEDGERS), std::runtime_error);
        // validator is at least here
        currentLedger += FEW_LEDGERS;

        REQUIRE(currentValidatorLedger() >= currentLedger);
        REQUIRE(currentListenerLedger() <= beforeGap);
    }

    simulation->stopAllNodes();
}

TEST_CASE("In quorum filtering", "[quorum][herder][acceptance]")
{
    auto mode = Simulation::OVER_LOOPBACK;
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);

    auto sim = Topologies::core(4, 0.75, mode, networkID, [](int i) {
        return getTestConfig(i, Config::TESTDB_ON_DISK_SQLITE);
    });

    sim->startAllNodes();

    // first, close ledgers with a simple topology Core0..Core3
    sim->crankUntil([&]() { return sim->haveAllExternalized(2, 1); },
                    std::chrono::seconds(1), false);

    // add a few extra validators, only connected to node 0
    // E_0 [3: Core0..Core3]
    // E_1 [3: Core0..Core3]
    // E_2 [3: Core0..Core3]
    // E_3 [3: Core0..Core3 E_1]

    auto nodeIDs = sim->getNodeIDs();
    auto node0 = sim->getNode(nodeIDs[0]);
    auto qSetBase = node0->getConfig().QUORUM_SET;
    std::vector<SecretKey> extraK;
    std::vector<SCPQuorumSet> qSetK;
    for (int i = 0; i < 4; i++)
    {
        extraK.emplace_back(
            SecretKey::fromSeed(sha256("E_" + std::to_string(i))));
        qSetK.emplace_back(qSetBase);
        if (i == 3)
        {
            qSetK[i].validators.emplace_back(extraK[1].getPublicKey());
        }
        auto node = sim->addNode(extraK[i], qSetK[i]);
        node->start();
        sim->addConnection(extraK[i].getPublicKey(), nodeIDs[0]);
    }

    // as they are not in quorum -> their messages are not forwarded to other
    // core nodes but they still externalize

    sim->crankUntil([&]() { return sim->haveAllExternalized(3, 1); },
                    std::chrono::seconds(20), false);

    // process scp messages for each core node
    auto checkCoreNodes =
        [&](std::function<void(std::vector<SCPEnvelope> const&)> proc) {
            for (auto const& k : qSetBase.validators)
            {
                auto c = sim->getNode(k);
                HerderImpl& herder = *static_cast<HerderImpl*>(&c->getHerder());

                auto const& lcl =
                    c->getLedgerManager().getLastClosedLedgerHeader();
                auto state =
                    herder.getSCP().getCurrentState(lcl.header.ledgerSeq);
                proc(state);
            }
        };

    // none of the messages from the extra nodes should be present
    checkCoreNodes([&](std::vector<SCPEnvelope> const& envs) {
        for (auto const& e : envs)
        {
            bool r = std::find_if(
                         extraK.begin(), extraK.end(), [&](SecretKey const& s) {
                             return e.statement.nodeID == s.getPublicKey();
                         }) != extraK.end();
            REQUIRE(!r);
        }
    });

    // then, change the quorum set of node Core3 to also include "E_2" and "E_3"
    // E_1 .. E_3 are now part of the overall quorum
    // E_0 is still not

    auto node3Config = sim->getNode(nodeIDs[3])->getConfig();
    sim->removeNode(node3Config.NODE_SEED.getPublicKey());
    sim->crankUntil([&]() { return sim->haveAllExternalized(4, 1); },
                    std::chrono::seconds(20), false);

    node3Config.QUORUM_SET.validators.emplace_back(extraK[2].getPublicKey());
    node3Config.QUORUM_SET.validators.emplace_back(extraK[3].getPublicKey());

    auto node3 = sim->addNode(node3Config.NODE_SEED, node3Config.QUORUM_SET,
                              &node3Config);
    node3->start();

    // connect it back to the core nodes
    for (int i = 0; i < 3; i++)
    {
        sim->addConnection(nodeIDs[3], nodeIDs[i]);
    }

    sim->crankUntil([&]() { return sim->haveAllExternalized(6, 3); },
                    std::chrono::seconds(20), true);

    checkCoreNodes([&](std::vector<SCPEnvelope> const& envs) {
        // messages for E1..E3 are present, E0 is still filtered
        std::vector<bool> found;
        found.resize(extraK.size(), false);
        for (auto const& e : envs)
        {
            for (int i = 0; i <= 3; i++)
            {
                found[i] = found[i] ||
                           (e.statement.nodeID == extraK[i].getPublicKey());
            }
        }
        int actual =
            static_cast<int>(std::count(++found.begin(), found.end(), true));
        int expected = static_cast<int>(extraK.size() - 1);
        REQUIRE(actual == expected);
        REQUIRE(!found[0]);
    });
}
