// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
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
#include "overlay/OverlayMetrics.h"
#include "test/TxTests.h"
#include "transactions/OperationFrame.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Math.h"

#include "xdr/Stellar-ledger.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <fmt/format.h>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;

TEST_CASE("standalone", "[herder][acceptance]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig(0, Config::TESTDB_DEFAULT));

    cfg.MANUAL_CLOSE = false;
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
    setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) * feeMult + extraFee);
    getSignatures(tx).clear();
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
        REQUIRE(!txSet->checkValid(*app, 0, 0));
    }
    SECTION("order check")
    {
        txSet->sortForHash();

        SECTION("success")
        {
            REQUIRE(txSet->checkValid(*app, 0, 0));

            txSet->trimInvalid(*app, 0, 0);
            REQUIRE(txSet->checkValid(*app, 0, 0));
        }
        SECTION("out of order")
        {
            std::swap(txSet->mTransactions[0], txSet->mTransactions[1]);
            REQUIRE(!txSet->checkValid(*app, 0, 0));

            txSet->trimInvalid(*app, 0, 0);
            REQUIRE(txSet->checkValid(*app, 0, 0));
        }
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            auto newUser = TestAccount{*app, getAccount("doesnotexist")};
            txSet->add(newUser.tx({payment(root, 1)}));
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app, 0, 0));

            txSet->trimInvalid(*app, 0, 0);
            REQUIRE(txSet->checkValid(*app, 0, 0));
        }
        SECTION("sequence gap")
        {
            SECTION("gap after")
            {
                auto tx = accounts[0].tx({payment(accounts[0], 1)});
                setSeqNum(tx, tx->getSeqNum() + 5);
                txSet->add(tx);
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app, 0, 0));

                txSet->trimInvalid(*app, 0, 0);
                REQUIRE(txSet->checkValid(*app, 0, 0));
            }
            SECTION("gap begin")
            {
                txSet->removeTx(txSet->sortForApply()[0]);
                txSet->sortForHash();
                REQUIRE(!txSet->checkValid(*app, 0, 0));

                auto removed = txSet->trimInvalid(*app, 0, 0);
                REQUIRE(txSet->checkValid(*app, 0, 0));
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
                REQUIRE(!txSet->checkValid(*app, 0, 0));

                auto removed = txSet->trimInvalid(*app, 0, 0);
                REQUIRE(txSet->checkValid(*app, 0, 0));
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
            REQUIRE(!txSet->checkValid(*app, 0, 0));

            auto removed = txSet->trimInvalid(*app, 0, 0);
            REQUIRE(txSet->checkValid(*app, 0, 0));
            REQUIRE(removed.size() == (nbTransactions + 1));
            REQUIRE(txSet->mTransactions.size() == nbTransactions);
        }
        SECTION("bad signature")
        {
            auto tx = std::static_pointer_cast<TransactionFrame>(
                txSet->mTransactions[0]);
            auto& tb = tx->getEnvelope().type() == ENVELOPE_TYPE_TX_V0
                           ? tx->getEnvelope().v0().tx.timeBounds.activate()
                           : tx->getEnvelope().v1().tx.timeBounds.activate();
            tb.maxTime = UINT64_MAX;
            tx->clearCached();
            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
    }
}

static TransactionFrameBasePtr
transaction(Application& app, TestAccount& account, int64_t sequenceDelta,
            int64_t amount, uint32_t fee)
{
    return transactionFromOperations(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), amount)}, fee);
}

static TransactionFrameBasePtr
feeBump(Application& app, TestAccount& feeSource, TransactionFrameBasePtr tx,
        int64_t fee)
{
    REQUIRE(tx->getEnvelope().type() == ENVELOPE_TYPE_TX);
    TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
    fb.feeBump().tx.feeSource = toMuxedAccount(feeSource);
    fb.feeBump().tx.fee = fee;
    fb.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
    fb.feeBump().tx.innerTx.v1() = tx->getEnvelope().v1();

    auto hash = sha256(xdr::xdr_to_opaque(
        app.getNetworkID(), ENVELOPE_TYPE_TX_FEE_BUMP, fb.feeBump().tx));
    fb.feeBump().signatures.emplace_back(SignatureUtils::sign(feeSource, hash));
    return TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(),
                                                         fb);
}

static void
testTxSetWithFeeBumps(uint32 protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 14;
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    app->start();

    auto const minBalance0 = app->getLedgerManager().getLastMinBalance(0);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto txSet = std::make_shared<TxSetFrame>(
        app->getLedgerManager().getLastClosedLedgerHeader().hash);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    auto checkTrimCheck = [&](std::vector<TransactionFrameBasePtr> const& txs) {
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app, 0, 0));
        auto trimmedSet = txSet->trimInvalid(*app, 0, 0);
        std::sort(trimmedSet.begin(), trimmedSet.end());
        auto txsNormalized = txs;
        std::sort(txsNormalized.begin(), txsNormalized.end());
        REQUIRE(trimmedSet == txsNormalized);
        REQUIRE(txSet->checkValid(*app, 0, 0));
    };

    SECTION("insufficient balance")
    {
        SECTION("two fee bumps with same sources, second insufficient")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            txSet->add(fb2);

            checkTrimCheck({fb1, fb2});
        }

        SECTION("three fee bumps, one with different fee source, "
                "different first")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account3, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            txSet->add(fb2);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 =
                feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);
            txSet->add(fb3);

            checkTrimCheck({fb2, fb3});
        }

        SECTION("three fee bumps, one with different fee source, "
                "different second")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account3, tx2, 200);
            txSet->add(fb2);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 =
                feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);
            txSet->add(fb3);

            checkTrimCheck({fb1, fb2, fb3});
        }

        SECTION("three fee bumps, one with different fee source, "
                "different third")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            txSet->add(fb2);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 = feeBump(*app, account3, tx3, 200);
            txSet->add(fb3);

            checkTrimCheck({fb1, fb2, fb3});
        }

        SECTION("two fee bumps with same fee source but different source")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account2, 1, 1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            txSet->add(fb2);

            checkTrimCheck({fb1, fb2});
        }
    }

    SECTION("invalid transaction")
    {
        SECTION("one fee bump")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, minBalance2);
            txSet->add(fb1);

            checkTrimCheck({fb1});
        }

        SECTION("two fee bumps with same sources, first has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, minBalance2);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            txSet->add(fb2);

            checkTrimCheck({fb1, fb2});
        }

        // Compare against
        // "two fee bumps with same sources, second insufficient"
        SECTION("two fee bumps with same sources, second has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, minBalance2);
            txSet->add(fb2);

            checkTrimCheck({fb2});
        }

        // Compare against
        // "two fee bumps with same sources, second insufficient"
        SECTION("two fee bumps with same sources, second insufficient, "
                "second invalid by malformed operation")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, -1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            txSet->add(fb2);

            checkTrimCheck({fb2});
        }

        SECTION("two fee bumps with same fee source but different source, "
                "second has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account2, 1, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, minBalance2);
            txSet->add(fb2);

            checkTrimCheck({fb2});
        }

        SECTION("two fee bumps with same fee source but different source, "
                "second insufficient, second invalid by malformed operation")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account2, 1, -1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            txSet->add(fb2);

            checkTrimCheck({fb2});
        }

        SECTION("three fee bumps with same fee source, third insufficient, "
                "second invalid by malformed operation")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            txSet->add(fb1);
            auto tx2 = transaction(*app, account1, 2, -1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            txSet->add(fb2);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 =
                feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);
            txSet->add(fb3);

            checkTrimCheck({fb2, fb3});
        }
    }
}

TEST_CASE("txset", "[herder][txset]")
{
    SECTION("protocol 10")
    {
        testTxSet(10);
    }
    SECTION("protocol 13")
    {
        testTxSet(13);
    }
    SECTION("protocol current")
    {
        testTxSet(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
        testTxSetWithFeeBumps(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
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
        REQUIRE(txSet->checkValid(*app, 0, 0));

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
        txSet->trimInvalid(*app, 0, 0);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
    };

    SECTION("basic single account")
    {
        auto refSeqNum = root.getLastSequenceNumber();
        addRootTxs();
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app, 0, 0));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app, 0, 0));
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
            setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) - 1);
            getSignatures(tx).clear();
            tx->addSignature(accountB);
            txSet->add(tx);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app, 0, 0));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app, 0, 0));
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
            REQUIRE(rTx->getNumOperations() == n + 1);
            REQUIRE(tx->getNumOperations() == n + 2);
            // use the same fee
            setFee(tx, static_cast<uint32_t>(rTx->getFeeBid()));
            getSignatures(tx).clear();
            tx->addSignature(accountB);
            txSet->add(tx);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app, 0, 0));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app, 0, 0));
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
                setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) - 1);
            }
            else
            {
                setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) + 1);
            }
            getSignatures(tx).clear();
            tx->addSignature(accountB);
            txSet->add(tx);
        }
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app, 0, 0));
        surgePricing();
        REQUIRE(txSet->size(lhCopy) == expectedReduced);
        REQUIRE(txSet->checkValid(*app, 0, 0));
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
        REQUIRE(txSet->checkValid(*app, 0, 0));
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
    SECTION("max 0 ops per ledger")
    {
        Config cfg(getTestConfig());
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 0;

        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        app->start();

        auto root = TestAccount::createRoot(*app);

        auto destAccount = root.create("destAccount", 500000000);
        auto accountB = root.create("accountB", 5000000000);
        auto accountC = root.create("accountC", 5000000000);

        TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        auto tx = makeMultiPayment(destAccount, root, 1, 100, 0, 1);
        txSet->add(tx);
        txSet->sortForHash();

        // txSet contains a valid transaction
        auto inv = txSet->trimInvalid(*app, 0, 0);
        REQUIRE(inv.empty());

        REQUIRE(txSet->sizeOp() == 1);
        // txSet is itself invalid as it's over the limit
        REQUIRE(!txSet->checkValid(*app, 0, 0));
        txSet->surgePricingFilter(*app);

        REQUIRE(txSet->sizeOp() == 0);
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->sizeOp() == 0);
        REQUIRE(txSet->checkValid(*app, 0, 0));
    }
}

static void
testSCPDriver(uint32 protocolVersion, uint32_t maxTxSize, size_t expectedOps)
{
    using SVUpgrades = decltype(StellarValue::upgrades);

    Config cfg(getTestConfig(0, Config::TESTDB_DEFAULT));

    cfg.MANUAL_CLOSE = false;
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
    auto makeTxUpgradePair = [&](HerderImpl& herder, TxSetFramePtr txSet,
                                 uint64_t closeTime,
                                 SVUpgrades const& upgrades) {
        txSet->sortForHash();
        StellarValue sv = herder.makeStellarValue(
            txSet->getContentsHash(), closeTime, upgrades, root.getSecretKey());
        auto v = xdr::xdr_to_opaque(sv);
        return TxPair{v, txSet};
    };
    auto makeTxPair = [&](HerderImpl& herder, TxSetFramePtr txSet,
                          uint64_t closeTime) {
        return makeTxUpgradePair(herder, txSet, closeTime, emptyUpgradeSteps);
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
        REQUIRE(result->checkValid(*app, 0, 0));
        return result;
    };

    SECTION("combineCandidates")
    {
        auto& herder = static_cast<HerderImpl&>(app->getHerder());

        ValueWrapperPtrSet candidates;

        auto addToCandidates = [&](TxPair const& p) {
            auto envelope = makeEnvelope(
                herder, p, {}, herder.getCurrentLedgerSeq() + 1, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p.second->getContentsHash(), *p.second));
            auto v = herder.getHerderSCPDriver().wrapValue(p.first);
            candidates.emplace(v);
        };

        struct CandidateSpec
        {
            int const n;
            int const nbOps;
            uint32 const feeMulti;
            TimePoint const closeTime;
            optional<uint32> const baseFeeIncrement;
        };

        std::vector<Hash> txSetHashes;
        std::vector<size_t> txSetSizes;
        std::vector<size_t> txSetOpSizes;
        std::vector<TimePoint> closeTimes;
        std::vector<decltype(lcl.header.baseFee)> baseFees;

        auto addCandidateThenTest = [&](CandidateSpec const& spec) {
            // Create a transaction set using the given parameters, combine
            // it with the given closeTime and optionally a given base fee
            // increment, and make it into a StellarValue to add to the list
            // of candidates so far.  Keep track of the hashes and sizes and
            // operation sizes of all the transaction sets, all of the close
            // times, and all of the base fee upgrades that we've seen, so that
            // we can compute the expected result of combining all the
            // candidates so far.  (We're using base fees simply as one example
            // of a type of upgrade, whose expected result is the maximum of all
            // candidates'.)
            TxSetFramePtr txSet =
                makeTransactions(lcl.hash, spec.n, spec.nbOps, spec.feeMulti);
            txSetHashes.push_back(txSet->getContentsHash());
            txSetSizes.push_back(txSet->size(lcl.header));
            txSetOpSizes.push_back(txSet->sizeOp());
            closeTimes.push_back(spec.closeTime);
            if (spec.baseFeeIncrement)
            {
                auto const baseFee =
                    lcl.header.baseFee + *spec.baseFeeIncrement;
                baseFees.push_back(baseFee);
                LedgerUpgrade ledgerUpgrade;
                ledgerUpgrade.type(LEDGER_UPGRADE_BASE_FEE);
                ledgerUpgrade.newBaseFee() = baseFee;
                Value upgrade(xdr::xdr_to_opaque(ledgerUpgrade));
                SVUpgrades upgrades;
                upgrades.emplace_back(upgrade.begin(), upgrade.end());
                addToCandidates(
                    makeTxUpgradePair(herder, txSet, spec.closeTime, upgrades));
            }
            else
            {
                addToCandidates(makeTxPair(herder, txSet, spec.closeTime));
            }

            // Compute the expected transaction set, close time, and upgrade
            // vector resulting from combining all the candidates so far.
            auto const bestTxSetIndex = std::distance(
                txSetSizes.begin(),
                std::max_element(txSetSizes.begin(), txSetSizes.end()));
            REQUIRE(txSetSizes.size() == closeTimes.size());
            auto const expectedHash = txSetHashes[bestTxSetIndex];
            auto const expectedCloseTime = closeTimes[bestTxSetIndex];
            SVUpgrades expectedUpgradeVector;
            if (!baseFees.empty())
            {
                LedgerUpgrade expectedLedgerUpgrade;
                expectedLedgerUpgrade.type(LEDGER_UPGRADE_BASE_FEE);
                expectedLedgerUpgrade.newBaseFee() =
                    *std::max_element(baseFees.begin(), baseFees.end());
                Value const expectedUpgradeValue(
                    xdr::xdr_to_opaque(expectedLedgerUpgrade));
                expectedUpgradeVector.emplace_back(expectedUpgradeValue.begin(),
                                                   expectedUpgradeValue.end());
            }

            // Combine all the candidates seen so far, and extract the
            // returned StellarValue.
            ValueWrapperPtr v =
                herder.getHerderSCPDriver().combineCandidates(1, candidates);
            StellarValue sv;
            xdr::xdr_from_opaque(v->getValue(), sv);

            // Compare the returned StellarValue's contents with the
            // expected ones that we computed above.
            REQUIRE(sv.ext.v() == STELLAR_VALUE_SIGNED);
            REQUIRE(sv.txSetHash == expectedHash);
            REQUIRE(sv.closeTime == expectedCloseTime);
            REQUIRE(sv.upgrades == expectedUpgradeVector);
        };

        // Test some list of candidates, comparing the output of
        // combineCandidates() and the one we compute at each step.

        std::vector<CandidateSpec> const specs{
            {0, 1, 100, 10, make_optional<uint32>()},
            {10, 1, 100, 5, make_optional<uint32>(1)},
            {5, 3, 100, 20, make_optional<uint32>(2)},
            {7, 2, 5, 30, make_optional<uint32>(3)}};

        std::for_each(specs.begin(), specs.end(), addCandidateThenTest);

        auto const bestTxSetIndex = std::distance(
            txSetSizes.begin(),
            std::max_element(txSetSizes.begin(), txSetSizes.end()));
        REQUIRE(txSetOpSizes[bestTxSetIndex] == expectedOps);

        TxSetFramePtr txSetL = makeTransactions(lcl.hash, maxTxSize, 1, 101);
        addToCandidates(makeTxPair(herder, txSetL, 20));
        TxSetFramePtr txSetL2 = makeTransactions(lcl.hash, maxTxSize, 1, 1000);
        addToCandidates(makeTxPair(herder, txSetL2, 20));
        auto v = herder.getHerderSCPDriver().combineCandidates(1, candidates);
        StellarValue sv;
        xdr::xdr_from_opaque(v->getValue(), sv);
        REQUIRE(sv.ext.v() == STELLAR_VALUE_SIGNED);
        REQUIRE(sv.txSetHash == txSetL2->getContentsHash());
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
            auto p = makeTxPair(herder, txSet0, ct);
            auto envelope = makeEnvelope(herder, p, {}, seq, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(txSet0->getContentsHash(), *txSet0));
        }

        SECTION("valid")
        {
            auto nomV = makeTxPair(herder, txSet0, ct);
            REQUIRE(scp.validateValue(seq, nomV.first, true) ==
                    SCPDriver::kFullyValidatedValue);

            auto balV = makeTxPair(herder, txSet0, ct);
            REQUIRE(scp.validateValue(seq, balV.first, false) ==
                    SCPDriver::kFullyValidatedValue);
        }
        SECTION("invalid")
        {
            auto checkInvalid = [&](StellarValue const& sv, bool nomination) {
                auto v = xdr::xdr_to_opaque(sv);
                REQUIRE(scp.validateValue(seq, v, nomination) ==
                        SCPDriver::kInvalidValue);
            };

            auto testInvalidValue = [&](bool isNomination) {
                SECTION("basic value")
                {
                    auto basicVal =
                        StellarValue(txSet0->getContentsHash(), ct,
                                     emptyUpgradeSteps, STELLAR_VALUE_BASIC);
                    checkInvalid(basicVal, isNomination);
                }
                SECTION("signed value")
                {
                    auto p = makeTxPair(herder, txSet0, ct);
                    StellarValue sv;
                    xdr::xdr_from_opaque(p.first, sv);

                    // mutate in a few ways
                    SECTION("missing signature")
                    {
                        sv.ext.lcValueSignature().signature.clear();
                        checkInvalid(sv, isNomination);
                    }
                    SECTION("wrong signature")
                    {
                        sv.ext.lcValueSignature().signature[0] ^= 1;
                        checkInvalid(sv, isNomination);
                    }
                    SECTION("wrong signature 2")
                    {
                        sv.ext.lcValueSignature().nodeID.ed25519()[0] ^= 1;
                        checkInvalid(sv, isNomination);
                    }
                }
            };

            SECTION("nomination")
            {
                testInvalidValue(/* isNomination */ true);
            }
            SECTION("ballot")
            {
                testInvalidValue(/* isNomination */ false);
            }
        }
    }

    SECTION("validateValue closeTimes")
    {
        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto& scp = herder.getHerderSCPDriver();

        auto const lclCloseTime = lcl.header.scpValue.closeTime;

        auto testTxBounds = [&](TimePoint const minTime,
                                TimePoint const maxTime,
                                TimePoint const nextCloseTime,
                                bool const expectValid) {
            REQUIRE(nextCloseTime > lcl.header.scpValue.closeTime);
            // Build a transaction set containing one transaction (which could
            // be any transaction that is valid in all ways aside from its time
            // bounds) with the given minTime and maxTime.
            auto tx = makeMultiPayment(root, root, 10, 1000, 0, 100);
            auto& tb = tx->getEnvelope().type() == ENVELOPE_TYPE_TX_V0
                           ? tx->getEnvelope().v0().tx.timeBounds.activate()
                           : tx->getEnvelope().v1().tx.timeBounds.activate();
            tb.minTime = minTime;
            tb.maxTime = maxTime;
            auto& sig = tx->getEnvelope().type() == ENVELOPE_TYPE_TX_V0
                            ? tx->getEnvelope().v0().signatures
                            : tx->getEnvelope().v1().signatures;
            sig.clear();
            tx->addSignature(root.getSecretKey());
            auto txSet = std::make_shared<TxSetFrame>(
                app->getLedgerManager().getLastClosedLedgerHeader().hash);
            txSet->add(tx);

            // Build a StellarValue containing the transaction set we just built
            // and the given next closeTime.
            auto val = makeTxPair(herder, txSet, nextCloseTime);
            auto const seq = herder.getCurrentLedgerSeq() + 1;
            auto envelope = makeEnvelope(herder, val, {}, seq, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(txSet->getContentsHash(), *txSet));

            // Validate the StellarValue.
            REQUIRE(scp.validateValue(seq, val.first, true) ==
                    (expectValid ? SCPDriver::kFullyValidatedValue
                                 : SCPDriver::kInvalidValue));

            // Confirm that trimInvalid() as used by
            // HerderImpl::triggerNextLedger() trims the transaction if and only
            // if we expect it to be invalid.
            auto closeTimeOffset = nextCloseTime - lclCloseTime;
            auto removed =
                txSet->trimInvalid(*app, closeTimeOffset, closeTimeOffset);
            REQUIRE(removed.size() == (expectValid ? 0 : 1));
        };

        auto t1 = lclCloseTime + 1, t2 = lclCloseTime + 2;

        SECTION("valid in all protocols")
        {
            testTxBounds(0, t1, t1, true);
        }

        SECTION("invalid time bounds: expired (invalid maxTime)")
        {
            testTxBounds(0, t1, t2, false);
        }

        SECTION("valid time bounds: premature minTime")
        {
            testTxBounds(t1, 0, t1, true);
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
        auto p1 = makeTxPair(herder, transactions1, 10);
        auto p2 = makeTxPair(herder, transactions1, 10);
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
    SECTION("protocol current")
    {
        testSCPDriver(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 1000, 15);
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

TEST_CASE("values externalized out of order", "[herder]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    auto validatorAKey = SecretKey::fromSeed(sha256("validator-A"));
    auto validatorBKey = SecretKey::fromSeed(sha256("validator-B"));
    auto validatorCKey = SecretKey::fromSeed(sha256("validator-C"));

    SCPQuorumSet qset;
    qset.threshold = 2;
    qset.validators.push_back(validatorAKey.getPublicKey());
    qset.validators.push_back(validatorBKey.getPublicKey());
    qset.validators.push_back(validatorCKey.getPublicKey());

    simulation->addNode(validatorAKey, qset);
    simulation->addNode(validatorBKey, qset);
    simulation->addNode(validatorCKey, qset);

    simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                     validatorCKey.getPublicKey());
    simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                     validatorBKey.getPublicKey());

    simulation->startAllNodes();
    auto A = simulation->getNode(validatorAKey.getPublicKey());
    auto B = simulation->getNode(validatorBKey.getPublicKey());
    auto C = simulation->getNode(validatorCKey.getPublicKey());

    auto currentALedger = [&]() {
        return A->getLedgerManager().getLastClosedLedgerNum();
    };
    auto currentCLedger = [&]() {
        return C->getLedgerManager().getLastClosedLedgerNum();
    };

    auto waitForLedgers = [&](int nLedgers) {
        auto destinationLedger = currentALedger() + nLedgers;
        simulation->crankUntil(
            [&]() {
                return simulation->haveAllExternalized(destinationLedger, 100);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        return std::min(currentALedger(), currentCLedger());
    };

    auto waitForA = [&](int nLedgers) {
        auto destinationLedger = currentALedger() + nLedgers;
        simulation->crankUntil(
            [&]() { return currentALedger() >= destinationLedger; },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        return currentALedger();
    };

    uint32_t currentLedger = 1;
    REQUIRE(currentALedger() == currentLedger);
    REQUIRE(currentCLedger() == currentLedger);

    // All nodes externalize a few ledgers
    auto fewLedgers = A->getConfig().MAX_SLOTS_TO_REMEMBER / 2;
    currentLedger = waitForLedgers(fewLedgers);

    // C is at most a ledger behind
    REQUIRE(currentALedger() >= currentLedger);
    REQUIRE(currentCLedger() == currentLedger);

    // disconnect C
    simulation->dropConnection(validatorAKey.getPublicKey(),
                               validatorCKey.getPublicKey());

    HerderImpl& herderA = *static_cast<HerderImpl*>(&A->getHerder());
    HerderImpl& herderB = *static_cast<HerderImpl*>(&B->getHerder());
    HerderImpl& herderC = *static_cast<HerderImpl*>(&C->getHerder());

    auto const& lmC = C->getLedgerManager();
    auto const& cmC = C->getCatchupManager();

    // Now construct a few future externalize messages
    // Make sure out of order messages are still within the validity range
    std::map<uint32_t, std::pair<SCPEnvelope, TxSetFramePtr>>
        validatorSCPMessagesA;
    std::map<uint32_t, std::pair<SCPEnvelope, TxSetFramePtr>>
        validatorSCPMessagesB;

    // Advance A and B a bit further, and collect externalize messages
    auto destinationLedger = waitForA(4);
    for (auto start = currentLedger + 1; start <= destinationLedger; start++)
    {
        for (auto const& env : herderA.getSCP().getLatestMessagesSend(start))
        {
            if (env.statement.pledges.type() == SCP_ST_EXTERNALIZE)
            {
                StellarValue sv;
                auto& pe = herderA.getPendingEnvelopes();
                herderA.getHerderSCPDriver().toStellarValue(
                    env.statement.pledges.externalize().commit.value, sv);
                auto txset = pe.getTxSet(sv.txSetHash);
                REQUIRE(txset);
                validatorSCPMessagesA[start] = std::make_pair(env, txset);
            }
        }

        for (auto const& env : herderB.getSCP().getLatestMessagesSend(start))
        {
            if (env.statement.pledges.type() == SCP_ST_EXTERNALIZE)
            {
                StellarValue sv;
                auto& pe = herderB.getPendingEnvelopes();
                herderB.getHerderSCPDriver().toStellarValue(
                    env.statement.pledges.externalize().commit.value, sv);
                auto txset = pe.getTxSet(sv.txSetHash);
                REQUIRE(txset);
                validatorSCPMessagesB[start] = std::make_pair(env, txset);
            }
        }
    }

    REQUIRE(validatorSCPMessagesA.size() == validatorSCPMessagesB.size());
    REQUIRE(herderC.getCurrentLedgerSeq() == currentCLedger());
    REQUIRE(currentCLedger() == currentLedger);

    auto testOutOfOrder = [&](bool partial) {
        auto first = currentLedger + 1;
        auto second = first + 1;
        auto third = second + 1;

        // Externalize future ledger
        // This should trigger CatchupManager to start buffering ledgers
        auto futureSlotA = validatorSCPMessagesA[third + 1];
        auto futureSlotB = validatorSCPMessagesB[third + 1];

        REQUIRE(herderC.recvSCPEnvelope(futureSlotA.first, qset,
                                        *(futureSlotA.second)) ==
                Herder::ENVELOPE_STATUS_READY);
        REQUIRE(herderC.recvSCPEnvelope(futureSlotB.first, qset,
                                        *(futureSlotB.second)) ==
                Herder::ENVELOPE_STATUS_READY);

        // Drop A-B connection, so that the network can't make progress
        simulation->dropConnection(validatorAKey.getPublicKey(),
                                   validatorBKey.getPublicKey());

        // Wait until C goes out of sync
        simulation->crankUntil([&]() { return !lmC.isSynced(); },
                               2 * Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS,
                               false);

        // Ensure LM is out of sync, and Herder tracks ledger seq from latest
        // envelope
        REQUIRE(herderC.getCurrentLedgerSeq() ==
                futureSlotA.first.statement.slotIndex);

        // Next, externalize a contiguous ledger
        // This will cause LM to apply it, and catchup manager will try to apply
        // buffered ledgers
        // complete - all messages are received out of order
        // partial - only most recent ledger is received out of order
        // CatchupManager should apply buffered ledgers and let LM get back
        // in sync
        std::vector<uint32_t> ledgers{first, third, second};
        if (partial)
        {
            ledgers = {first, second, third};
        }

        for (size_t i = 0; i < ledgers.size(); i++)
        {
            auto slotA = validatorSCPMessagesA[ledgers[i]];
            auto slotB = validatorSCPMessagesB[ledgers[i]];

            REQUIRE(
                herderC.recvSCPEnvelope(slotA.first, qset, *(slotA.second)) ==
                Herder::ENVELOPE_STATUS_READY);
            REQUIRE(
                herderC.recvSCPEnvelope(slotB.first, qset, *(slotB.second)) ==
                Herder::ENVELOPE_STATUS_READY);

            REQUIRE(herderC.getCurrentLedgerSeq() ==
                    futureSlotA.first.statement.slotIndex);

            REQUIRE(!cmC.isCatchupInitialized());

            // At the last ledger, LM is back in sync
            if (i == ledgers.size() - 1)
            {
                REQUIRE(lmC.isSynced());
                REQUIRE(!cmC.hasBufferedLedger());
            }
            else
            {
                REQUIRE(!lmC.isSynced());
            }
        }

        // As we're back in sync now, ensure Herder and LM are consistent with
        // each other
        auto lcl = lmC.getLastClosedLedgerNum();
        REQUIRE(lcl == herderC.getCurrentLedgerSeq());

        // Ensure that C sent out a nomination message for the next consensus
        // round
        simulation->crankUntil(
            [&]() {
                for (auto const& msg :
                     herderC.getSCP().getLatestMessagesSend(lcl + 1))
                {
                    if (msg.statement.pledges.type() == SCP_ST_NOMINATE)
                    {
                        return true;
                    }
                }
                return false;
            },
            2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    };

    SECTION("in order")
    {
        for (auto const& msgPair : validatorSCPMessagesA)
        {
            auto msgA = msgPair.second;
            auto msgB = validatorSCPMessagesB[msgPair.first];

            REQUIRE(herderC.recvSCPEnvelope(msgA.first, qset, *(msgA.second)) ==
                    Herder::ENVELOPE_STATUS_READY);
            REQUIRE(herderC.recvSCPEnvelope(msgB.first, qset, *(msgB.second)) ==
                    Herder::ENVELOPE_STATUS_READY);

            // Tracking is updated correctly
            REQUIRE(herderC.getCurrentLedgerSeq() ==
                    msgA.first.statement.slotIndex);
            // nothing out of ordinary in LM
            REQUIRE(lmC.isSynced());
            // Catchup is not running, no ledgers are buffered
            REQUIRE(!cmC.hasBufferedLedger());
        }
    }
    SECTION("completely out of order")
    {
        testOutOfOrder(/* partial */ false);
    }
    SECTION("partially out of order")
    {
        testOutOfOrder(/* partial */ true);
    }
    SECTION("C goes back in sync and unsticks the network")
    {
        testOutOfOrder(/* partial */ false);

        // Now that C is back in sync and triggered next ledger
        // (and B is disconnected), C and A should be able to make progress
        simulation->addConnection(validatorAKey.getPublicKey(),
                                  validatorCKey.getPublicKey());

        auto lcl = currentALedger();
        auto nextLedger = lcl + fewLedgers;

        // Make sure A and C are starting from the same ledger
        REQUIRE(lcl == currentCLedger());

        waitForA(fewLedgers);
        REQUIRE(currentALedger() == nextLedger);
        REQUIRE(currentCLedger() == nextLedger);
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

    auto cfg1 = getTestConfig(1);
    auto cfg2 = getTestConfig(2);
    cfg1.MAX_SLOTS_TO_REMEMBER = 5;
    cfg2.MAX_SLOTS_TO_REMEMBER = cfg1.MAX_SLOTS_TO_REMEMBER;

    simulation->addNode(validatorKey, qSet, &cfg1);
    simulation->addNode(listenerKey, qSet, &cfg2);
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

    auto app = simulation->getNode(listenerKey.getPublicKey());
    // we pick SMALL_GAP to be as close to the maximum number of ledgers that
    // are kept in memory, with room for the watcher node to be behind by one
    // ledger
    auto static const SMALL_GAP = app->getConfig().MAX_SLOTS_TO_REMEMBER - 1;
    // BIG_GAP, we just need to pick a number greater than what we keep in
    // memory
    auto static const BIG_GAP = app->getConfig().MAX_SLOTS_TO_REMEMBER + 1;

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
    auto checkCoreNodes = [&](std::function<bool(SCPEnvelope const&)> proc) {
        for (auto const& k : qSetBase.validators)
        {
            auto c = sim->getNode(k);
            HerderImpl& herder = *static_cast<HerderImpl*>(&c->getHerder());

            auto const& lcl = c->getLedgerManager().getLastClosedLedgerHeader();
            herder.getSCP().processCurrentState(lcl.header.ledgerSeq, proc,
                                                true);
        }
    };

    // none of the messages from the extra nodes should be present
    checkCoreNodes([&](SCPEnvelope const& e) {
        bool r =
            std::find_if(extraK.begin(), extraK.end(), [&](SecretKey const& s) {
                return e.statement.nodeID == s.getPublicKey();
            }) != extraK.end();
        REQUIRE(!r);
        return true;
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

    std::vector<bool> found;
    found.resize(extraK.size(), false);

    checkCoreNodes([&](SCPEnvelope const& e) {
        // messages for E1..E3 are present, E0 is still filtered
        for (int i = 0; i <= 3; i++)
        {
            found[i] =
                found[i] || (e.statement.nodeID == extraK[i].getPublicKey());
        }
        return true;
    });
    int actual =
        static_cast<int>(std::count(++found.begin(), found.end(), true));
    int expected = static_cast<int>(extraK.size() - 1);
    REQUIRE(actual == expected);
    REQUIRE(!found[0]);
}

static void
externalize(SecretKey const& sk, LedgerManager& lm, HerderImpl& herder,
            std::vector<TransactionFrameBasePtr> const& txs)
{
    auto const& lcl = lm.getLastClosedLedgerHeader();
    auto ledgerSeq = lcl.header.ledgerSeq + 1;

    auto txSet = std::make_shared<TxSetFrame>(lcl.hash);
    for (auto const& tx : txs)
    {
        txSet->add(tx);
    }
    herder.getPendingEnvelopes().putTxSet(txSet->getContentsHash(), ledgerSeq,
                                          txSet);

    StellarValue sv = herder.makeStellarValue(
        txSet->getContentsHash(), 2, xdr::xvector<UpgradeType, 6>{}, sk);
    herder.getHerderSCPDriver().valueExternalized(ledgerSeq,
                                                  xdr::xdr_to_opaque(sv));
}

TEST_CASE("do not flood invalid transactions", "[herder]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FLOOD_TX_PERIOD_MS = 0;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    auto& tq = herder.getTransactionQueue();

    auto root = TestAccount::createRoot(*app);
    auto acc = root.create("A", lm.getLastMinBalance(2));

    auto tx1a = acc.tx({payment(acc, 1)});
    auto tx1r = root.tx({bumpSequence(INT64_MAX)});
    // this will be invalid after tx1r gets applied
    auto tx2r = root.tx({payment(root, 1)});

    herder.recvTransaction(tx1a);
    herder.recvTransaction(tx1r);
    herder.recvTransaction(tx2r);

    size_t numBroadcast = 0;
    tq.mTxBroadcastedEvent = [&](TransactionFrameBasePtr&) { ++numBroadcast; };

    externalize(cfg.NODE_SEED, lm, herder, {tx1r});
    REQUIRE(numBroadcast == 1);

    auto const& lhhe = lm.getLastClosedLedgerHeader();
    auto txSet = tq.toTxSet(lhhe);
    REQUIRE(txSet->mTransactions.size() == 1);
    REQUIRE(txSet->mTransactions.front()->getContentsHash() ==
            tx1a->getContentsHash());
    REQUIRE(txSet->checkValid(*app, 0, 0));
}

TEST_CASE("do not flood too many transactions", "[herder][transactionqueue]")
{
    auto test = [](bool delayed, uint32_t numOps) {
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto simulation = std::make_shared<Simulation>(
            Simulation::OVER_LOOPBACK, networkID, [&](int i) {
                auto cfg = getTestConfig(i);
                cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 500;
                cfg.NODE_IS_VALIDATOR = false;
                cfg.FORCE_SCP = false;
                cfg.FLOOD_TX_PERIOD_MS = delayed ? 100 : 0;
                cfg.FLOOD_OP_RATE_PER_LEDGER = 2.0;
                return cfg;
            });

        auto mainKey = SecretKey::fromSeed(sha256("main"));
        auto otherKey = SecretKey::fromSeed(sha256("other"));

        SCPQuorumSet qset;
        qset.threshold = 1;
        qset.validators.push_back(mainKey.getPublicKey());

        simulation->addNode(mainKey, qset);
        simulation->addNode(otherKey, qset);

        simulation->addPendingConnection(mainKey.getPublicKey(),
                                         otherKey.getPublicKey());
        simulation->startAllNodes();
        simulation->crankForAtLeast(std::chrono::seconds(1), false);

        auto app = simulation->getNode(mainKey.getPublicKey());
        auto const& cfg = app->getConfig();
        auto& lm = app->getLedgerManager();
        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto& tq = herder.getTransactionQueue();

        auto root = TestAccount::createRoot(*app);
        std::vector<TestAccount> accs;

        // number of accounts to use
        int const nbAccounts = 40;
        // number of transactions to generate per fee
        // groups are
        int const feeGroupMaxSize = 7;
        // used to track fee
        int feeGroupSize = 0;
        uint32 curFeeOffset = 10000;

        accs.reserve(nbAccounts);
        for (int i = 0; i < nbAccounts; ++i)
        {
            accs.emplace_back(
                root.create(fmt::format("A{}", i), lm.getLastMinBalance(2)));
        }
        std::deque<uint32> fees;

        auto genTx = [&](TestAccount& source, uint32_t numOps, bool highFee) {
            std::vector<Operation> ops;
            for (int64_t i = 1; i <= numOps; ++i)
            {
                ops.emplace_back(payment(source, i));
            }
            auto tx = source.tx(ops);
            auto txFee = static_cast<uint32_t>(tx->getFeeBid());
            if (highFee)
            {
                txFee += 100000;
                fees.emplace_front(txFee);
            }
            else
            {
                txFee += curFeeOffset;
                fees.emplace_back(txFee);
            }
            setFee(tx, txFee);
            getSignatures(tx).clear();
            tx->addSignature(source.getSecretKey());
            if (++feeGroupSize == feeGroupMaxSize)
            {
                feeGroupSize = 0;
                curFeeOffset--;
            }

            REQUIRE(herder.recvTransaction(tx) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            return tx;
        };

        auto genTxRandAccount = [&](uint32_t numOps) {
            genTx(rand_element(accs), numOps, false);
        };

        size_t const maxOps = cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE;

        auto tx1a = genTx(accs[0], numOps, false);
        auto tx1r = genTx(root, numOps, false);
        size_t numTx = 2;
        for (; (numTx + 2) * numOps <= maxOps; ++numTx)
        {
            genTxRandAccount(numOps);
        }

        std::map<AccountID, SequenceNumber> bcastTracker;
        TransactionFrameBasePtr lastTx;
        tq.mTxBroadcastedEvent = [&](TransactionFrameBasePtr& tx) {
            // ensure that sequence numbers are correct per account
            auto expected = tx->getSeqNum();
            std::swap(bcastTracker[tx->getSourceID()], expected);
            if (expected != 0)
            {
                expected++;
                REQUIRE(expected == tx->getSeqNum());
            }
            // check if we have the expected fee
            REQUIRE(tx->getFeeBid() == fees.front());
            fees.pop_front();
        };

        REQUIRE(tq.toTxSet({})->mTransactions.size() == numTx);

        // remove the first two transactions that won't be
        // re-broadcasted during externalize
        fees.pop_front();
        fees.pop_front();

        size_t numBroadcast = 0;
        tq.mTxBroadcastedEvent = [&](TransactionFrameBasePtr&) {
            ++numBroadcast;
        };

        externalize(cfg.NODE_SEED, lm, herder, {tx1a, tx1r});

        if (delayed)
        {
            // no broadcast right away
            REQUIRE(numBroadcast == 0);
            // wait for a bit more than a broadcast period
            // rate per period is
            // 2*(maxOps=500)*(FLOOD_TX_PERIOD_MS=100)/((ledger time=5)*1000)
            // 1000*100/5000=20
            auto constexpr opsRatePerPeriod = 20;
            auto broadcastPeriod =
                std::chrono::milliseconds(cfg.FLOOD_TX_PERIOD_MS);
            auto const delta = std::chrono::milliseconds(1);
            simulation->crankForAtLeast(broadcastPeriod + delta, false);

            if (numOps <= opsRatePerPeriod)
            {
                auto opsBroadcasted = numBroadcast * numOps;
                // goal reached
                REQUIRE(opsBroadcasted <= opsRatePerPeriod);
                // an extra tx would have exceeded the limit
                REQUIRE(opsBroadcasted + numOps > opsRatePerPeriod);
            }
            else
            {
                // can only flood up to 1 transaction per cycle
                REQUIRE(numBroadcast <= 1);
            }
            // as we're waiting for a ledger worth of capacity
            // and we have a multiplier of 2
            // it should take about half a ledger period to broadcast everything

            // we wait a bit more, and inject an extra high fee transaction
            // from an account with no pending transactions
            // this transactions should be the next one to be broadcasted
            simulation->crankForAtLeast(std::chrono::milliseconds(500), false);
            genTx(root, numOps, true);

            simulation->crankForAtLeast(std::chrono::milliseconds(2000), false);
            REQUIRE(numBroadcast == (numTx - 1));
            REQUIRE(tq.toTxSet({})->mTransactions.size() == numTx - 1);
        }
        else
        {
            REQUIRE(numBroadcast == (numTx - 2));
            REQUIRE(tq.toTxSet({})->mTransactions.size() == numTx - 2);
            // check that there is no broadcast after that
            simulation->crankForAtLeast(std::chrono::seconds(1), false);
            REQUIRE(numBroadcast == (numTx - 2));
            REQUIRE(tq.toTxSet({})->mTransactions.size() == numTx - 2);
        }
        simulation->stopAllNodes();
    };

    auto testOps = [&](bool delayed) {
        SECTION("one operation per transaction")
        {
            test(delayed, 1);
        }
        SECTION("a few operations per transaction")
        {
            test(delayed, 7);
        }
        SECTION("full transactions")
        {
            test(delayed, 100);
        }
    };
    SECTION("no delay")
    {
        testOps(false);
    }
    SECTION("delayed")
    {
        testOps(true);
    }
}

TEST_CASE("slot herder policy", "[herder]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    Config cfg(getTestConfig());

    // start in sync
    cfg.FORCE_SCP = true;
    cfg.MANUAL_CLOSE = false;
    cfg.NODE_SEED = v0SecretKey;
    cfg.MAX_SLOTS_TO_REMEMBER = 5;
    cfg.NODE_IS_VALIDATOR = false;

    cfg.QUORUM_SET.threshold = 3; // 3 out of 4
    cfg.QUORUM_SET.validators.push_back(v1NodeID);
    cfg.QUORUM_SET.validators.push_back(v2NodeID);
    cfg.QUORUM_SET.validators.push_back(v3NodeID);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto qSet = herder.getSCP().getLocalQuorumSet();
    auto qsetHash = sha256(xdr::xdr_to_opaque(qSet));

    auto recvExternalize = [&](SecretKey const& sk, uint64_t slotIndex,
                               Hash const& prevHash) {
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotIndex;
        envelope.statement.pledges.type(SCP_ST_EXTERNALIZE);
        auto& ext = envelope.statement.pledges.externalize();
        TxSetFramePtr txSet = std::make_shared<TxSetFrame>(prevHash);

        // sign values with the same secret key
        StellarValue sv = herder.makeStellarValue(
            txSet->getContentsHash(), (TimePoint)slotIndex,
            xdr::xvector<UpgradeType, 6>{}, v1SecretKey);
        ext.commit.counter = 1;
        ext.commit.value = xdr::xdr_to_opaque(sv);
        ext.commitQuorumSetHash = qsetHash;
        ext.nH = 1;
        envelope.statement.nodeID = sk.getPublicKey();
        herder.signEnvelope(sk, envelope);
        auto res = herder.recvSCPEnvelope(envelope, qSet, *txSet);
        REQUIRE(res == Herder::ENVELOPE_STATUS_READY);
    };

    auto const LIMIT = cfg.MAX_SLOTS_TO_REMEMBER;

    auto recvExternPeers = [&](uint32 seq, Hash const& prev, bool quorum) {
        recvExternalize(v1SecretKey, seq, prev);
        recvExternalize(v2SecretKey, seq, prev);
        if (quorum)
        {
            recvExternalize(v3SecretKey, seq, prev);
        }
    };
    // first, close a few ledgers, see if we actually retain the right
    // number of ledgers
    auto timeout = clock.now() + std::chrono::minutes(10);
    for (uint32 i = 0; i < LIMIT * 2; ++i)
    {
        auto seq = app->getLedgerManager().getLastClosedLedgerNum() + 1;
        auto prev = app->getLedgerManager().getLastClosedLedgerHeader().hash;
        recvExternPeers(seq, prev, true);
        while (app->getLedgerManager().getLastClosedLedgerNum() < seq)
        {
            clock.crank(true);
            REQUIRE(clock.now() < timeout);
        }
    }
    REQUIRE(herder.getState() == Herder::HERDER_TRACKING_STATE);
    REQUIRE(herder.getSCP().getKnownSlotsCount() == LIMIT);

    auto oneSec = std::chrono::seconds(1);
    // let the node go out of sync, it should reach the desired state
    timeout = clock.now() + Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS + oneSec;
    while (herder.getState() == Herder::HERDER_TRACKING_STATE)
    {
        clock.crank(false);
        REQUIRE(clock.now() < timeout);
    }

    auto const PARTIAL = Herder::LEDGER_VALIDITY_BRACKET;
    // create a gap
    auto newSeq = app->getLedgerManager().getLastClosedLedgerNum() + 2;
    for (uint32 i = 0; i < PARTIAL; ++i)
    {
        auto prev = app->getLedgerManager().getLastClosedLedgerHeader().hash;
        // advance clock to ensure that ct is valid
        clock.sleep_for(oneSec);
        recvExternPeers(newSeq++, prev, false);
    }
    REQUIRE(herder.getSCP().getKnownSlotsCount() == (LIMIT + PARTIAL));

    timeout = clock.now() + Herder::OUT_OF_SYNC_RECOVERY_TIMER + oneSec;
    while (herder.getSCP().getKnownSlotsCount() !=
           Herder::LEDGER_VALIDITY_BRACKET)
    {
        clock.sleep_for(oneSec);
        clock.crank(false);
        REQUIRE(clock.now() < timeout);
    }

    Hash prevHash;
    // add a bunch more - not v-blocking
    for (uint32 i = 0; i < LIMIT; ++i)
    {
        recvExternalize(v1SecretKey, newSeq++, prevHash);
    }
    // policy here is to not do anything
    auto waitForRecovery = [&]() {
        timeout = clock.now() + Herder::OUT_OF_SYNC_RECOVERY_TIMER + oneSec;
        while (clock.now() < timeout)
        {
            clock.sleep_for(oneSec);
            clock.crank(false);
        }
    };

    waitForRecovery();
    auto const FULLSLOTS = Herder::LEDGER_VALIDITY_BRACKET + LIMIT;
    REQUIRE(herder.getSCP().getKnownSlotsCount() == FULLSLOTS);

    // now inject a few more, policy should apply here, with
    // partial in between
    // lower slots getting dropped so the total number of slots in memory is
    // constant
    auto cutOff = Herder::LEDGER_VALIDITY_BRACKET - 1;
    for (uint32 i = 0; i < cutOff; ++i)
    {
        recvExternPeers(newSeq++, prevHash, false);
        waitForRecovery();
        REQUIRE(herder.getSCP().getKnownSlotsCount() == FULLSLOTS);
    }
    // adding one more, should get rid of the partial slots
    recvExternPeers(newSeq++, prevHash, false);
    waitForRecovery();
    REQUIRE(herder.getSCP().getKnownSlotsCount() ==
            Herder::LEDGER_VALIDITY_BRACKET);
}

TEST_CASE("exclude transactions by operation type", "[herder]")
{
    SECTION("operation is received when no filter")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        Application::pointer app = createTestApplication(clock, cfg);
        app->start();

        auto root = TestAccount::createRoot(*app);
        auto acc = getAccount("acc");
        auto tx = root.tx({createAccount(acc.getPublicKey(), 1)});

        REQUIRE(app->getHerder().recvTransaction(tx) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);
    }

    SECTION("filter excludes transaction containing specified operation")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE = {CREATE_ACCOUNT};
        Application::pointer app = createTestApplication(clock, cfg);
        app->start();

        auto root = TestAccount::createRoot(*app);
        auto acc = getAccount("acc");
        auto tx = root.tx({createAccount(acc.getPublicKey(), 1)});

        REQUIRE(app->getHerder().recvTransaction(tx) ==
                TransactionQueue::AddResult::ADD_STATUS_FILTERED);
    }

    SECTION("filter does not exclude transaction containing non-specified "
            "operation")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE = {MANAGE_DATA};
        Application::pointer app = createTestApplication(clock, cfg);
        app->start();

        auto root = TestAccount::createRoot(*app);
        auto acc = getAccount("acc");
        auto tx = root.tx({createAccount(acc.getPublicKey(), 1)});

        REQUIRE(app->getHerder().recvTransaction(tx) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);
    }
}
