// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/test/TestTxSetUtils.h"
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
#include "herder/HerderUtils.h"
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
#include "util/ProtocolVersion.h"

#include "crypto/Hex.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/autocheck.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <fmt/format.h>
#include <optional>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;

TEST_CASE_VERSIONS("standalone", "[herder][acceptance]")
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
                REQUIRE(app->getHerder().recvTransaction(tx, false) ==
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
                    hasC = protocolVersionStartsFrom(
                        ltx.loadHeader().current().ledgerVersion,
                        ProtocolVersion::V_10);
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
                // b1's last transaction failed due to account non existent
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

static TransactionFramePtr
makeSelfPayment(stellar::TestAccount& account, int nbOps, uint32_t fee)
{
    std::vector<stellar::Operation> ops;
    for (int i = 0; i < nbOps; i++)
    {
        ops.emplace_back(payment(account, i + 1000));
    }
    auto tx = account.tx(ops);
    setFee(tx, fee);
    getSignatures(tx).clear();
    tx->addSignature(account);
    return tx;
}

static void
testTxSet(uint32 protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 14;
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    // set up world
    auto root = TestAccount::createRoot(*app);

    const int nbAccounts = 2;
    const int nbTransactions = 5;

    auto accounts = std::vector<TestAccount>{};

    const int64_t minBalance0 = app->getLedgerManager().getLastMinBalance(0);

    // amount only allows up to nbTransactions
    int64_t amountPop =
        nbTransactions * app->getLedgerManager().getLastTxFee() + minBalance0;

    std::vector<TransactionFrameBasePtr> txs;
    auto genTx = [&](int nbTxs) {
        std::string accountName = fmt::format("A{}", accounts.size());
        accounts.push_back(root.create(accountName.c_str(), amountPop));
        auto& account = accounts.back();
        for (int j = 0; j < nbTxs; j++)
        {
            // payment to self
            txs.push_back(account.tx({payment(account.getPublicKey(), 10000)}));
        }
    };
    for (size_t i = 0; i < nbAccounts; i++)
    {
        genTx(nbTransactions);
    }
    SECTION("valid set")
    {
        auto txSet = TxSetFrame::makeFromTransactions(txs, *app, 0, 0);
        REQUIRE(txSet->sizeTx() == (2 * nbTransactions));
    }

    SECTION("too many txs")
    {
        while (txs.size() <= cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE * 2)
        {
            genTx(1);
        }
        auto txSet = TxSetFrame::makeFromTransactions(txs, *app, 0, 0);
        REQUIRE(txSet->sizeTx() == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            auto newUser = TestAccount{*app, getAccount("doesnotexist")};
            txs.push_back(newUser.tx({payment(root, 1)}));
            TxSetFrame::Transactions removed;
            auto txSet =
                TxSetFrame::makeFromTransactions(txs, *app, 0, 0, &removed);
            REQUIRE(removed.size() == 1);
            REQUIRE(txSet->sizeTx() == (2 * nbTransactions));
        }
        SECTION("sequence gap")
        {
            SECTION("gap after")
            {
                auto tx = accounts[0].tx({payment(accounts[0], 1)});
                setSeqNum(tx, tx->getSeqNum() + 5);
                txs.push_back(tx);

                TxSetFrame::Transactions removed;
                auto txSet =
                    TxSetFrame::makeFromTransactions(txs, *app, 0, 0, &removed);
                REQUIRE(removed.size() == 1);
                REQUIRE(txSet->sizeTx() == (2 * nbTransactions));
            }
            SECTION("gap begin")
            {
                txs.erase(txs.begin());

                TxSetFrame::Transactions removed;
                auto txSet =
                    TxSetFrame::makeFromTransactions(txs, *app, 0, 0, &removed);

                // one of the account lost all its transactions
                REQUIRE(removed.size() == (nbTransactions - 1));
                REQUIRE(txSet->sizeTx() == nbTransactions);
            }
            SECTION("gap middle")
            {
                int remIdx = 2; // 3rd transaction from the first account
                txs.erase(txs.begin() + remIdx);

                TxSetFrame::Transactions removed;
                auto txSet =
                    TxSetFrame::makeFromTransactions(txs, *app, 0, 0, &removed);

                // one account has all its transactions,
                // the other, we removed transactions after remIdx
                auto expectedRemoved = nbTransactions - remIdx - 1;
                REQUIRE(removed.size() == expectedRemoved);
                REQUIRE(txSet->sizeTx() ==
                        (nbTransactions * 2 - expectedRemoved - 1));
            }
        }
        SECTION("insufficient balance")
        {
            // extra transaction would push the account below the reserve
            txs.push_back(accounts[0].tx({payment(accounts[0], 10)}));

            TxSetFrame::Transactions removed;
            auto txSet =
                TxSetFrame::makeFromTransactions(txs, *app, 0, 0, &removed);
            REQUIRE(removed.size() == (nbTransactions + 1));
            REQUIRE(txSet->sizeTx() == nbTransactions);
        }
        SECTION("bad signature")
        {
            auto tx = std::static_pointer_cast<TransactionFrame>(txs[0]);
            setMaxTime(tx, UINT64_MAX);
            tx->clearCached();
            TxSetFrame::Transactions removed;
            auto txSet =
                TxSetFrame::makeFromTransactions(txs, *app, 0, 0, &removed);
            REQUIRE(removed.size() == nbTransactions);
            REQUIRE(txSet->sizeTx() == nbTransactions);
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

static void
testTxSetWithFeeBumps(uint32 protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 14;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto const minBalance0 = app->getLedgerManager().getLastMinBalance(0);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    auto compareTxs = [](TxSetFrame::Transactions const& actual,
                         TxSetFrame::Transactions const& expected) {
        auto actualNormalized = actual;
        auto expectedNormalized = expected;
        std::sort(actualNormalized.begin(), actualNormalized.end());
        std::sort(expectedNormalized.begin(), expectedNormalized.end());
        REQUIRE(actualNormalized == expectedNormalized);
    };

    SECTION("insufficient balance")
    {
        SECTION("two fee bumps with same sources, second insufficient")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb1, fb2});
        }

        SECTION("three fee bumps, one with different fee source, "
                "different first")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account3, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 =
                feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2, fb3}, *app,
                                                          0, 0, &invalidTxs);
            compareTxs(invalidTxs, {fb2, fb3});
        }

        SECTION("three fee bumps, one with different fee source, "
                "different second")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account3, tx2, 200);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 =
                feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);

            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2, fb3}, *app,
                                                          0, 0, &invalidTxs);
            compareTxs(invalidTxs, {fb1, fb2, fb3});
        }

        SECTION("three fee bumps, one with different fee source, "
                "different third")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 = feeBump(*app, account3, tx3, 200);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2, fb3}, *app,
                                                          0, 0, &invalidTxs);
            compareTxs(invalidTxs, {fb1, fb2, fb3});
        }

        SECTION("two fee bumps with same fee source but different source")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account2, 1, 1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb1, fb2});
        }
    }

    SECTION("invalid transaction")
    {
        SECTION("one fee bump")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, minBalance2);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1}, *app, 0, 0,
                                                          &invalidTxs);
            compareTxs(invalidTxs, {fb1});
        }

        SECTION("two fee bumps with same sources, first has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, minBalance2);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb1, fb2});
        }

        // Compare against
        // "two fee bumps with same sources, second insufficient"
        SECTION("two fee bumps with same sources, second has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, minBalance2);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb2});
        }

        // Compare against
        // "two fee bumps with same sources, second insufficient"
        SECTION("two fee bumps with same sources, second insufficient, "
                "second invalid by malformed operation")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, -1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb2});
        }

        SECTION("two fee bumps with same fee source but different source, "
                "second has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account2, 1, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, minBalance2);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb2});
        }

        SECTION("two fee bumps with same fee source but different source, "
                "second insufficient, second invalid by malformed operation")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account2, 1, -1, 100);
            auto fb2 =
                feeBump(*app, account2, tx2, minBalance2 - minBalance0 - 199);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2}, *app, 0,
                                                          0, &invalidTxs);
            compareTxs(invalidTxs, {fb2});
        }

        SECTION("three fee bumps with same fee source, third insufficient, "
                "second invalid by malformed operation")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account1, 2, -1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            auto tx3 = transaction(*app, account1, 3, 1, 100);
            auto fb3 =
                feeBump(*app, account2, tx3, minBalance2 - minBalance0 - 199);
            TxSetFrame::Transactions invalidTxs;
            auto txSet = TxSetFrame::makeFromTransactions({fb1, fb2, fb3}, *app,
                                                          0, 0, &invalidTxs);
            compareTxs(invalidTxs, {fb2, fb3});
        }
    }
}

TEST_CASE("txset", "[herder][txset]")
{
    SECTION("protocol 13")
    {
        testTxSet(13);
    }
    SECTION("generalized tx set protocol")
    {
        testTxSet(static_cast<uint32>(GENERALIZED_TX_SET_PROTOCOL_VERSION));
    }
    SECTION("protocol current")
    {
        testTxSet(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
        testTxSetWithFeeBumps(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    }
}

TEST_CASE_VERSIONS("txset with PreconditionsV2", "[herder][txset]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
    auto root = TestAccount::createRoot(*app);
    auto a1 = root.create("a1", minBalance2);

    for_versions_to(18, *app, [&] {
        auto checkTxSupport = [&](PreconditionsV2 const& c) {
            auto tx = transactionWithV2Precondition(*app, a1, 1, 100, c);
            TxSetFrame::Transactions invalidTxs;
            auto txSet =
                TxSetFrame::makeFromTransactions({tx}, *app, 0, 0, &invalidTxs);
            REQUIRE(invalidTxs.size() == 1);
            REQUIRE(tx->getResultCode() == txNOT_SUPPORTED);
        };

        SECTION("empty V2 precondition")
        {
            PreconditionsV2 cond;
            checkTxSupport(cond);
        }
        SECTION("ledgerBounds")
        {
            PreconditionsV2 cond;
            LedgerBounds b;
            cond.ledgerBounds.activate() = b;
            checkTxSupport(cond);
        }
        SECTION("minSeqNum")
        {
            PreconditionsV2 cond;
            cond.minSeqNum.activate() = 0;
            checkTxSupport(cond);
        }
        SECTION("minSeqLedgerGap")
        {
            PreconditionsV2 cond;
            cond.minSeqLedgerGap = 1;
            checkTxSupport(cond);
        }
        SECTION("minSeqAge")
        {
            PreconditionsV2 cond;
            cond.minSeqAge = 1;
            checkTxSupport(cond);
        }
        SECTION("extraSigners")
        {
            SignerKey rootSigner;
            rootSigner.type(SIGNER_KEY_TYPE_ED25519);
            rootSigner.ed25519() = root.getPublicKey().ed25519();

            PreconditionsV2 cond;
            cond.extraSigners.emplace_back(rootSigner);
            checkTxSupport(cond);
        }
    });

    for_versions_from(19, *app, [&] {
        // Move close time past 0
        closeLedgerOn(*app, 1, 1, 2022);

        SECTION("minSeqNum gap")
        {
            auto minSeqNumCond = [](SequenceNumber seqNum) {
                PreconditionsV2 cond;
                cond.minSeqNum.activate() = seqNum;
                return cond;
            };

            auto tx1 = transaction(*app, a1, 1, 1, 100);
            auto tx2InvalidGap = transactionWithV2Precondition(
                *app, a1, 5, 100,
                minSeqNumCond(a1.getLastSequenceNumber() + 2));
            TxSetFrame::Transactions removed;
            auto txSet = TxSetFrame::makeFromTransactions({tx1, tx2InvalidGap},
                                                          *app, 0, 0, &removed);
            REQUIRE(removed.back() == tx2InvalidGap);

            auto tx2 = transactionWithV2Precondition(
                *app, a1, 5, 100,
                minSeqNumCond(a1.getLastSequenceNumber() + 1));
            auto tx3 = transaction(*app, a1, 6, 1, 100);
            removed.clear();
            txSet = TxSetFrame::makeFromTransactions({tx1, tx2, tx3}, *app, 0,
                                                     0, &removed);

            REQUIRE(removed.empty());
        }
        SECTION("minSeqLedgerGap")
        {
            auto minSeqLedgerGapCond = [](uint32_t minSeqLedgerGap) {
                PreconditionsV2 cond;
                cond.minSeqLedgerGap = minSeqLedgerGap;
                return cond;
            };

            auto test = [&](bool v3ExtIsSet, bool minSeqNumTxIsFeeBump) {
                // gap between a1's seqLedger and lcl
                uint32_t minGap;
                if (v3ExtIsSet)
                {
                    // run a v19 op so a1's seqLedger is set
                    a1.bumpSequence(0);
                    closeLedger(*app);
                    closeLedger(*app);
                    minGap = 2;
                }
                else
                {
                    // a1 seqLedger is 0 because it has not done a
                    // v19 tx yet
                    minGap = app->getLedgerManager().getLastClosedLedgerNum();
                }

                auto txInvalid = transactionWithV2Precondition(
                    *app, a1, 1, 100, minSeqLedgerGapCond(minGap + 2));
                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions({txInvalid}, *app,
                                                              0, 0, &removed);

                REQUIRE(removed.back() == txInvalid);
                REQUIRE(txSet->sizeTx() == 0);

                // we use minGap lcl + 1 because validation is done against
                // the next ledger
                auto tx1 = transactionWithV2Precondition(
                    *app, a1, 1, 100, minSeqLedgerGapCond(minGap + 1));

                // only the first tx can have minSeqLedgerGap set
                auto tx2Invalid = transactionWithV2Precondition(
                    *app, a1, 2, 100, minSeqLedgerGapCond(minGap + 1));

                auto fb1 = feeBump(*app, a1, tx1, 200);
                auto fb2Invalid = feeBump(*app, a1, tx2Invalid, 200);
                removed.clear();
                if (minSeqNumTxIsFeeBump)
                {
                    txSet = TxSetFrame::makeFromTransactions(
                        {fb1, fb2Invalid}, *app, 0, 0, &removed);
                }
                else
                {
                    txSet = TxSetFrame::makeFromTransactions(
                        {tx1, tx2Invalid}, *app, 0, 0, &removed);
                }

                REQUIRE(removed.size() == 1);
                REQUIRE(removed.back() ==
                        (minSeqNumTxIsFeeBump ? fb2Invalid : tx2Invalid));
            };

            SECTION("before v3 ext is set")
            {
                test(false, false);
            }
            SECTION("after v3 ext is set")
            {
                test(true, false);
            }
            SECTION("after v3 ext is set - fee bump")
            {
                test(true, true);
            }
        }
        SECTION("minSeqAge")
        {
            auto minSeqAgeCond = [](Duration minSeqAge) {
                PreconditionsV2 cond;
                cond.minSeqAge = minSeqAge;
                return cond;
            };

            auto test = [&](bool v3ExtIsSet, bool minSeqNumTxIsFeeBump) {
                Duration minGap;
                if (v3ExtIsSet)
                {
                    // run a v19 op so a1's seqLedger is set
                    a1.bumpSequence(0);
                    closeLedgerOn(
                        *app,
                        app->getLedgerManager().getLastClosedLedgerNum() + 1,
                        app->getLedgerManager()
                                .getLastClosedLedgerHeader()
                                .header.scpValue.closeTime +
                            1);
                    minGap = 1;
                }
                else
                {
                    minGap = app->getLedgerManager()
                                 .getLastClosedLedgerHeader()
                                 .header.scpValue.closeTime;
                }

                auto txInvalid = transactionWithV2Precondition(
                    *app, a1, 1, 100, minSeqAgeCond(minGap + 1));
                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions({txInvalid}, *app,
                                                              0, 0, &removed);
                REQUIRE(removed.back() == txInvalid);
                REQUIRE(txSet->sizeTx() == 0);

                auto tx1 = transactionWithV2Precondition(*app, a1, 1, 100,
                                                         minSeqAgeCond(minGap));

                // only the first tx can have minSeqAge set
                auto tx2Invalid = transactionWithV2Precondition(
                    *app, a1, 2, 100, minSeqAgeCond(minGap));

                auto fb1 = feeBump(*app, a1, tx1, 200);
                auto fb2Invalid = feeBump(*app, a1, tx2Invalid, 200);

                removed.clear();
                if (minSeqNumTxIsFeeBump)
                {
                    txSet = TxSetFrame::makeFromTransactions(
                        {fb1, fb2Invalid}, *app, 0, 0, &removed);
                }
                else
                {
                    txSet = TxSetFrame::makeFromTransactions(
                        {tx1, tx2Invalid}, *app, 0, 0, &removed);
                }

                REQUIRE(removed.size() == 1);
                REQUIRE(removed.back() ==
                        (minSeqNumTxIsFeeBump ? fb2Invalid : tx2Invalid));

                REQUIRE(txSet->checkValid(*app, 0, 0));
            };
            SECTION("before v3 ext is set")
            {
                test(false, false);
            }
            SECTION("after v3 ext is set")
            {
                test(true, false);
            }
            SECTION("after v3 ext is set - fee bump")
            {
                test(true, true);
            }
        }
        SECTION("ledgerBounds")
        {
            auto ledgerBoundsCond = [](uint32_t minLedger, uint32_t maxLedger) {
                LedgerBounds bounds;
                bounds.minLedger = minLedger;
                bounds.maxLedger = maxLedger;

                PreconditionsV2 cond;
                cond.ledgerBounds.activate() = bounds;
                return cond;
            };

            auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();

            auto tx1 = transaction(*app, a1, 1, 1, 100);

            SECTION("minLedger")
            {
                auto txInvalid = transactionWithV2Precondition(
                    *app, a1, 2, 100, ledgerBoundsCond(lclNum + 2, 0));
                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions(
                    {tx1, txInvalid}, *app, 0, 0, &removed);
                REQUIRE(removed.back() == txInvalid);

                // the highest minLedger can be is lcl + 1 because
                // validation is done against the next ledger
                auto tx2 = transactionWithV2Precondition(
                    *app, a1, 2, 100, ledgerBoundsCond(lclNum + 1, 0));
                removed.clear();
                txSet = TxSetFrame::makeFromTransactions({tx1, tx2}, *app, 0, 0,
                                                         &removed);
                REQUIRE(removed.empty());
            }
            SECTION("maxLedger")
            {
                auto txInvalid = transactionWithV2Precondition(
                    *app, a1, 2, 100, ledgerBoundsCond(0, lclNum));
                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions(
                    {tx1, txInvalid}, *app, 0, 0, &removed);
                REQUIRE(removed.back() == txInvalid);

                // the lower maxLedger can be is lcl + 2, as the current
                // ledger is lcl + 1 and maxLedger bound is exclusive.
                auto tx2 = transactionWithV2Precondition(
                    *app, a1, 2, 100, ledgerBoundsCond(0, lclNum + 2));
                removed.clear();
                txSet = TxSetFrame::makeFromTransactions({tx1, tx2}, *app, 0, 0,
                                                         &removed);
                REQUIRE(removed.empty());
            }
        }
        SECTION("extraSigners")
        {
            SignerKey rootSigner;
            rootSigner.type(SIGNER_KEY_TYPE_ED25519);
            rootSigner.ed25519() = root.getPublicKey().ed25519();

            PreconditionsV2 cond;
            cond.extraSigners.emplace_back(rootSigner);

            SECTION("one extra signer")
            {
                auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
                SECTION("success")
                {
                    tx->addSignature(root.getSecretKey());
                    TxSetFrame::Transactions removed;
                    auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0,
                                                                  0, &removed);
                    REQUIRE(removed.empty());
                }
                SECTION("fail")
                {
                    TxSetFrame::Transactions removed;
                    auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0,
                                                                  0, &removed);
                    REQUIRE(removed.back() == tx);
                }
            }
            SECTION("two extra signers")
            {
                auto a2 = root.create("a2", minBalance2);

                SignerKey a2Signer;
                a2Signer.type(SIGNER_KEY_TYPE_ED25519);
                a2Signer.ed25519() = a2.getPublicKey().ed25519();

                cond.extraSigners.emplace_back(a2Signer);
                auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
                tx->addSignature(root.getSecretKey());

                SECTION("success")
                {
                    tx->addSignature(a2.getSecretKey());
                    TxSetFrame::Transactions removed;
                    auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0,
                                                                  0, &removed);
                    REQUIRE(removed.empty());
                }
                SECTION("fail")
                {
                    TxSetFrame::Transactions removed;
                    auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0,
                                                                  0, &removed);
                    REQUIRE(removed.back() == tx);
                }
            }
            SECTION("duplicate extra signers")
            {
                cond.extraSigners.emplace_back(rootSigner);
                auto txDupeSigner =
                    transactionWithV2Precondition(*app, a1, 1, 100, cond);
                txDupeSigner->addSignature(root.getSecretKey());
                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions(
                    {txDupeSigner}, *app, 0, 0, &removed);
                REQUIRE(removed.back() == txDupeSigner);
                REQUIRE(txDupeSigner->getResultCode() == txMALFORMED);
            }
            SECTION("signer overlap with default account signer")
            {
                auto rootTx =
                    transactionWithV2Precondition(*app, root, 1, 100, cond);
                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions({rootTx}, *app, 0,
                                                              0, &removed);
                REQUIRE(removed.empty());
            }
            SECTION("signer overlap with added account signer")
            {
                auto sk1 = makeSigner(root, 100);
                a1.setOptions(setSigner(sk1));

                auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
                SECTION("signature present")
                {
                    tx->addSignature(root.getSecretKey());

                    TxSetFrame::Transactions removed;
                    auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0,
                                                                  0, &removed);
                    REQUIRE(removed.empty());
                }
                SECTION("signature missing")
                {
                    TxSetFrame::Transactions removed;
                    auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0,
                                                                  0, &removed);
                    REQUIRE(removed.back() == tx);
                }
            }
            SECTION("signer overlap with added account signer - both "
                    "signers used")
            {
                auto sk1 = makeSigner(root, 100);
                a1.setOptions(setSigner(sk1));

                auto tx = transactionFrameFromOps(app->getNetworkID(), a1,
                                                  {root.op(payment(a1, 1))},
                                                  {root}, cond);

                TxSetFrame::Transactions removed;
                auto txSet = TxSetFrame::makeFromTransactions({tx}, *app, 0, 0,
                                                              &removed);
                REQUIRE(removed.empty());
            }
        }
    });
}

TEST_CASE("txset base fee", "[herder][txset]")
{
    Config cfg(getTestConfig());
    uint32_t const maxTxSetSize = 112;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSetSize;

    auto testBaseFee = [&](uint32_t protocolVersion, uint32 nbTransactions,
                           uint32 extraAccounts, size_t lim, int64_t expLowFee,
                           int64_t expHighFee,
                           uint32_t expNotChargedAccounts = 0) {
        cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

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

        std::vector<TransactionFrameBasePtr> txs;
        for (uint32 i = 0; i < nbTransactions; i++)
        {
            std::string nameI = fmt::format("Base{}", i);
            auto aI = root.create(nameI, startingBalance);
            accounts.push_back(aI);

            auto tx = makeMultiPayment(aI, aI, 1, 1000, 0, 10);
            txs.push_back(tx);
        }

        for (uint32 k = 1; k <= extraAccounts; k++)
        {
            std::string nameI = fmt::format("Extra{}", k);
            auto aI = root.create(nameI, startingBalance);
            accounts.push_back(aI);

            auto tx = makeMultiPayment(aI, aI, 2, 1000, k, 100);
            txs.push_back(tx);
        }
        TxSetFrameConstPtr txSet =
            TxSetFrame::makeFromTransactions(txs, *app, 0, 0);
        REQUIRE(txSet->size(lhCopy) == lim);
        REQUIRE(extraAccounts >= 2);

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
        closeLedger(*app, txSet);

        auto balancesAfter = getBalances();
        int64_t lowFee = INT64_MAX, highFee = 0;
        uint32_t notChargedAccounts = 0;
        for (size_t i = 0; i < balancesAfter.size(); i++)
        {
            auto b = balancesBefore[i];
            auto a = balancesAfter[i];
            auto fee = b - a;
            if (fee == 0)
            {
                ++notChargedAccounts;
                continue;
            }
            lowFee = std::min(lowFee, fee);
            highFee = std::max(highFee, fee);
        }

        REQUIRE(lowFee == expLowFee);
        REQUIRE(highFee == expHighFee);
        REQUIRE(notChargedAccounts == expNotChargedAccounts);
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
                testBaseFee(10, baseCount, v10ExtraTx, maxTxSetSize, 1000,
                            20104);
            }
            SECTION("protocol before generalized tx set")
            {
                // low = 10*base tx = baseFee = 1000
                // high = 2*base (surge)
                SECTION("maxed out surged")
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                                    1,
                                baseCount, v11ExtraTx, maxTxSetSize, 1000,
                                2000);
                }
                SECTION("smallest surged")
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                                    1,
                                baseCount + 1, v11ExtraTx - 50,
                                maxTxSetSize - 100 + 1, 1000, 2000);
                }
            }
            SECTION("generalized tx set protocol")
            {
                SECTION("fitting exactly into capacity does not cause surge")
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION),
                                baseCount, v11ExtraTx, maxTxSetSize, 100, 200);
                }
                SECTION("evicting one tx causes surge")
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION),
                                baseCount + 1, v11ExtraTx, maxTxSetSize, 1000,
                                2000, 1);
                }
            }
            SECTION("protocol current")
            {
                if (protocolVersionStartsFrom(
                        Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                        GENERALIZED_TX_SET_PROTOCOL_VERSION))
                {
                    SECTION(
                        "fitting exactly into capacity does not cause surge")
                    {
                        testBaseFee(static_cast<uint32_t>(
                                        GENERALIZED_TX_SET_PROTOCOL_VERSION),
                                    baseCount, v11ExtraTx, maxTxSetSize, 100,
                                    200);
                    }
                    SECTION("evicting one tx causes surge")
                    {
                        testBaseFee(static_cast<uint32_t>(
                                        GENERALIZED_TX_SET_PROTOCOL_VERSION),
                                    baseCount + 1, v11ExtraTx, maxTxSetSize,
                                    1000, 2000, 1);
                    }
                }
                else
                {
                    SECTION("maxed out surged")
                    {
                        testBaseFee(static_cast<uint32_t>(
                                        GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                                        1,
                                    baseCount, v11ExtraTx, maxTxSetSize, 1000,
                                    2000);
                    }
                    SECTION("smallest surged")
                    {
                        testBaseFee(static_cast<uint32_t>(
                                        GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                                        1,
                                    baseCount + 1, v11ExtraTx - 50,
                                    maxTxSetSize - 100 + 1, 1000, 2000);
                    }
                }
            }
        }
        SECTION("newOnly")
        {
            SECTION("protocol 10")
            {
                // low = 20000+1
                // high = 20000+112
                testBaseFee(10, 0, v10NewCount, maxTxSetSize, 20001, 20112);
            }
            SECTION("protocol before generalized tx set")
            {
                // low = 20000+1 -> baseFee = 20001/2+ = 10001
                // high = 10001*2
                testBaseFee(
                    static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                        1,
                    0, v11NewCount, maxTxSetSize, 20001, 20002);
            }
            SECTION("generalized tx set protocol")
            {
                SECTION("fitting exactly into capacity does not cause surge")
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION),
                                0, v11NewCount, maxTxSetSize, 200, 200);
                }
                SECTION("evicting one tx causes surge")
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION),
                                0, v11NewCount + 1, maxTxSetSize, 20002, 20002,
                                1);
                }
            }
            SECTION("protocol current")
            {
                if (protocolVersionStartsFrom(
                        Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                        GENERALIZED_TX_SET_PROTOCOL_VERSION))
                {
                    SECTION(
                        "fitting exactly into capacity does not cause surge")
                    {
                        testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                                    v11NewCount, maxTxSetSize, 200, 200);
                    }
                    SECTION("evicting one tx causes surge")
                    {
                        testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                                    v11NewCount + 1, maxTxSetSize, 20002, 20002,
                                    1);
                    }
                }
                else
                {
                    testBaseFee(static_cast<uint32_t>(
                                    GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                                    1,
                                0, v11NewCount, maxTxSetSize, 20001, 20002);
                }
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
                            v11ExtraTx - 50, maxTxSetSize - 100, 100, 200);
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
                            v11NewCount - 50, maxTxSetSize - 100, 200, 200);
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
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

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

    auto multiPaymentTx =
        std::bind(makeMultiPayment, destAccount, _1, _2, _3, 0, 100);

    auto refSeqNumRoot = root.getLastSequenceNumber();
    std::vector<TransactionFrameBasePtr> rootTxs;
    for (uint32_t n = 0; n < 2 * nbTxs; n++)
    {
        rootTxs.push_back(multiPaymentTx(root, n + 1, 10000 + 1000 * n));
    }

    SECTION("basic single account")
    {
        auto txSet = TxSetFrame::makeFromTransactions(rootTxs, *app, 0, 0);
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        // check that the expected tx are there
        auto txs = txSet->getTxsInApplyOrder();
        for (auto& tx : txs)
        {
            refSeqNumRoot++;
            REQUIRE(tx->getSeqNum() == refSeqNumRoot);
        }
    }

    SECTION("one account paying more")
    {
        for (uint32_t n = 0; n < nbTxs; n++)
        {
            auto tx = multiPaymentTx(accountB, n + 1, 10000 + 1000 * n);
            setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) - 1);
            getSignatures(tx).clear();
            tx->addSignature(accountB);
            rootTxs.push_back(tx);
        }
        auto txSet = TxSetFrame::makeFromTransactions(rootTxs, *app, 0, 0);
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        // check that the expected tx are there
        for (auto const& tx : txSet->getTxs())
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("one account with more operations but same total fee")
    {
        for (uint32_t n = 0; n < nbTxs; n++)
        {
            auto tx = multiPaymentTx(accountB, n + 2, 10000 + 1000 * n);
            // find corresponding root tx (should have 1 less op)
            auto rTx = rootTxs[n];
            REQUIRE(rTx->getNumOperations() == n + 1);
            REQUIRE(tx->getNumOperations() == n + 2);
            // use the same fee
            setFee(tx, static_cast<uint32_t>(rTx->getFeeBid()));
            getSignatures(tx).clear();
            tx->addSignature(accountB);
            rootTxs.push_back(tx);
        }
        auto txSet = TxSetFrame::makeFromTransactions(rootTxs, *app, 0, 0);
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        // check that the expected tx are there
        for (auto const& tx : txSet->getTxs())
        {
            REQUIRE(tx->getSourceID() == root.getPublicKey());
        }
    }

    SECTION("one account paying more except for one tx in middle")
    {
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
            rootTxs.push_back(tx);
        }
        auto txSet = TxSetFrame::makeFromTransactions(rootTxs, *app, 0, 0);
        REQUIRE(txSet->size(lhCopy) == expectedReduced);
        // check that the expected tx are there
        int nbAccountB = 0;
        for (auto const& tx : txSet->getTxsInApplyOrder())
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
            rootTxs.push_back(root.tx({payment(destAccount, n + 10)}));
            rootTxs.push_back(accountB.tx({payment(destAccount, n + 10)}));
            rootTxs.push_back(accountC.tx({payment(destAccount, n + 10)}));
        }
        auto txSet = TxSetFrame::makeFromTransactions(rootTxs, *app, 0, 0);
        REQUIRE(txSet->size(lhCopy) == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
        REQUIRE(txSet->checkValid(*app, 0, 0));
    }
}

TEST_CASE("surge pricing", "[herder][txset]")
{
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

        auto root = TestAccount::createRoot(*app);

        auto destAccount = root.create("destAccount", 500000000);
        auto tx = makeMultiPayment(destAccount, root, 1, 100, 0, 1);

        TxSetFrame::Transactions invalidTxs;
        TxSetFrameConstPtr txSet =
            TxSetFrame::makeFromTransactions({tx}, *app, 0, 0, &invalidTxs);

        // Transaction is valid, but trimmed by surge pricing.
        REQUIRE(invalidTxs.empty());
        REQUIRE(txSet->sizeTx() == 0);
    }
}

TEST_CASE("surge pricing with DEX separation", "[herder][txset]")
{
    if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                GENERALIZED_TX_SET_PROTOCOL_VERSION))
    {
        return;
    }
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 15;
    cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET = 5;

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerHeader lhCopy;
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        lhCopy = ltx.loadHeader().current();
    }

    auto root = TestAccount::createRoot(*app);

    auto accountA = root.create("accountA", 5000000000);
    auto accountB = root.create("accountB", 5000000000);
    auto accountC = root.create("accountC", 5000000000);

    auto seqNumA = accountA.getLastSequenceNumber();
    auto seqNumB = accountB.getLastSequenceNumber();
    auto seqNumC = accountC.getLastSequenceNumber();

    auto runTest = [&](std::vector<TransactionFrameBasePtr> const& txs,
                       size_t expectedTxsA, size_t expectedTxsB,
                       size_t expectedTxsC, int64_t expectedNonDexBaseFee,
                       int64_t expectedDexBaseFee) {
        auto txSet = TxSetFrame::makeFromTransactions(txs, *app, 0, 0);
        size_t cntA = 0, cntB = 0, cntC = 0;
        auto resTxs = txSet->getTxsInApplyOrder();
        for (auto const& tx : resTxs)
        {
            if (tx->getSourceID() == accountA.getPublicKey())
            {
                ++cntA;
                ++seqNumA;
                REQUIRE(seqNumA == tx->getSeqNum());
            }
            if (tx->getSourceID() == accountB.getPublicKey())
            {
                ++cntB;
                ++seqNumB;
                REQUIRE(seqNumB == tx->getSeqNum());
            }
            if (tx->getSourceID() == accountC.getPublicKey())
            {
                ++cntC;
                ++seqNumC;
                REQUIRE(seqNumC == tx->getSeqNum());
            }
            auto baseFee = txSet->getTxBaseFee(tx, lhCopy);
            REQUIRE(baseFee);
            if (tx->hasDexOperations())
            {
                REQUIRE(*baseFee == expectedDexBaseFee);
            }
            else
            {
                REQUIRE(*baseFee == expectedNonDexBaseFee);
            }
        }
        REQUIRE(cntA == expectedTxsA);
        REQUIRE(cntB == expectedTxsB);
        REQUIRE(cntC == expectedTxsC);
    };

    auto nonDexTx = [](TestAccount& account, int nbOps, uint32_t opFee) {
        return makeSelfPayment(account, nbOps, opFee * nbOps);
    };
    auto dexTx = [&](TestAccount& account, int nbOps, uint32_t opFee) {
        return createSimpleDexTx(*app, account, nbOps, opFee * nbOps);
    };

    SECTION("single account")
    {
        SECTION("only non DEX txs")
        {
            runTest({nonDexTx(accountA, 3, 200), nonDexTx(accountA, 4, 120),
                     nonDexTx(accountA, 2, 150), nonDexTx(accountA, 5, 250),
                     /* cutoff */ nonDexTx(accountA, 2, 300),
                     nonDexTx(accountA, 1, 500)},
                    4, 0, 0, 120, 0);
        }
        SECTION("only DEX txs")
        {
            runTest({dexTx(accountA, 3, 200), dexTx(accountA, 1, 120),
                     /* cutoff */
                     dexTx(accountA, 2, 150), dexTx(accountA, 1, 300)},
                    2, 0, 0, 0, 120);
        }
        SECTION("mixed txs")
        {
            SECTION("only DEX surge priced")
            {
                SECTION("DEX limit reached")
                {
                    runTest(
                        {/* 5 non-DEX ops + 4 DEX ops = 9 ops */
                         nonDexTx(accountA, 2, 200), dexTx(accountA, 4, 250),
                         nonDexTx(accountA, 1, 150),
                         /* cutoff */ dexTx(accountA, 2, 150),
                         nonDexTx(accountA, 1, 500)},
                        3, 0, 0, 100, 250);
                }
                SECTION("both limits reached")
                {
                    // DEX tx didn't fit into both DEX and global limits, but
                    // there are no remaining non-DEX txs to activate surge
                    // pricing for them.
                    runTest(
                        {
                            /* 10 non-DEX ops + 4 DEX ops = 14 ops */
                            nonDexTx(accountA, 10, 200),
                            dexTx(accountA, 4, 250),
                            /* cutoff */
                            dexTx(accountA, 2, 300),
                            nonDexTx(accountA, 1, 500),
                        },
                        2, 0, 0, 100, 250);
                }
            }
            SECTION("both DEX and non-dex surge priced")
            {
                SECTION("non-DEX fee is lowest")
                {
                    runTest(
                        {
                            /* 8 non-DEX ops + 4 DEX ops = 12 ops */
                            nonDexTx(accountA, 3, 200),
                            dexTx(accountA, 4, 250),
                            nonDexTx(accountA, 5, 150),
                            /* cutoff */
                            nonDexTx(accountA, 4, 500),
                            dexTx(accountA, 2, 150),
                        },
                        3, 0, 0, 150, 150);
                }
                SECTION("DEX fee is lowest")
                {
                    runTest(
                        {
                            /* 8 non-DEX ops + 4 DEX ops = 12 ops */
                            nonDexTx(accountA, 3, 200),
                            dexTx(accountA, 4, 150),
                            nonDexTx(accountA, 5, 250),
                            /* cutoff */
                            nonDexTx(accountA, 4, 500),
                            dexTx(accountA, 2, 150),
                        },
                        3, 0, 0, 150, 150);
                }
            }
        }
    }

    SECTION("multiple accounts")
    {
        SECTION("only non-DEX txs")
        {
            // Last 3 txs do not fit into limit and activate surge pricing.
            runTest({nonDexTx(accountA, 3, 200), nonDexTx(accountA, 5, 250),
                     nonDexTx(accountB, 4, 300), nonDexTx(accountC, 2, 400),
                     /* cutoff */
                     nonDexTx(accountA, 2, 500), nonDexTx(accountB, 2, 180),
                     nonDexTx(accountC, 2, 100)},
                    2, 1, 1, 200, 0);
        }
        SECTION("only DEX txs")
        {
            // Last two txs do not fit into DEX ops limit and activate surge
            // pricing.
            runTest({dexTx(accountA, 1, 200), dexTx(accountA, 1, 250),
                     dexTx(accountB, 1, 300), dexTx(accountC, 2, 400),
                     /* cutoff */
                     dexTx(accountA, 4, 500), dexTx(accountB, 1, 180),
                     dexTx(accountC, 1, 100)},
                    2, 1, 1, 0, 200);
        }
        SECTION("mixed txs")
        {
            SECTION("only DEX surge priced")
            {
                SECTION("DEX limit reached")
                {
                    runTest(
                        {
                            /* 6 non-DEX ops + 5 DEX ops = 11 ops */
                            nonDexTx(accountA, 1, 300),
                            dexTx(accountA, 2, 400),
                            dexTx(accountB, 1, 300),
                            nonDexTx(accountB, 2, 400),
                            dexTx(accountC, 2, 250),
                            nonDexTx(accountC, 3, 500),
                            /* cutoff */
                            dexTx(accountA, 1, 200),
                            dexTx(accountB, 1, 200),
                            dexTx(accountC, 1, 249),
                        },
                        2, 2, 2, 100, 250);
                }
                SECTION("both limits reached, but only DEX evicted")
                {
                    runTest(
                        {
                            /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                            nonDexTx(accountA, 2, 600),
                            dexTx(accountA, 3, 400),
                            nonDexTx(accountB, 3, 400),
                            dexTx(accountC, 2, 500),
                            nonDexTx(accountC, 5, 250),
                            /* cutoff */
                            dexTx(accountA, 1, 399),
                            dexTx(accountB, 1, 399),
                            dexTx(accountC, 1, 399),
                        },
                        2, 1, 2, 100, 400);
                }
            }
            SECTION("all txs surge priced")
            {
                SECTION("only global limit reached")
                {
                    runTest(
                        {
                            /* 13 non-DEX ops + 2 DEX ops = 15 ops */
                            nonDexTx(accountA, 6, 300),
                            dexTx(accountB, 1, 400),
                            nonDexTx(accountB, 3, 400),
                            nonDexTx(accountC, 4, 250),
                            dexTx(accountC, 1, 500),
                            /* cutoff */
                            dexTx(accountA, 1, 200),
                            nonDexTx(accountB, 1, 249),
                            dexTx(accountC, 1, 249),
                        },
                        1, 2, 2, 250, 250);
                }
                SECTION("both limits reached")
                {
                    SECTION("non-DEX fee is lowest")
                    {
                        runTest(
                            {
                                /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                                nonDexTx(accountA, 2, 600),
                                dexTx(accountA, 3, 400),
                                nonDexTx(accountB, 3, 400),
                                dexTx(accountC, 2, 500),
                                nonDexTx(accountC, 5, 250),
                                /* cutoff */
                                dexTx(accountA, 1, 399),
                                nonDexTx(accountB, 1, 249),
                            },
                            2, 1, 2, 250, 400);
                    }
                    SECTION("DEX fee is lowest")
                    {
                        runTest(
                            {
                                /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                                dexTx(accountA, 3, 300),
                                nonDexTx(accountA, 2, 500),
                                nonDexTx(accountB, 3, 400),
                                dexTx(accountC, 2, 200),
                                nonDexTx(accountC, 5, 250),
                                /* cutoff */
                                dexTx(accountA, 1, 199),
                                nonDexTx(accountB, 1, 199),
                            },
                            2, 1, 2, 200, 200);
                    }
                }
            }
        }
    }
}

TEST_CASE("surge pricing with DEX separation holds invariants",
          "[herder][txset]")
{
    if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                GENERALIZED_TX_SET_PROTOCOL_VERSION))
    {
        return;
    }

    auto runTest = [](std::optional<uint32_t> maxDexOps, int dexOpsPercent) {
        Config cfg(getTestConfig());
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 20;
        cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET = maxDexOps;
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        LedgerHeader lhCopy;
        {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            lhCopy = ltx.loadHeader().current();
        }

        uniform_int_distribution<> isDexTxDistr(0, 100);
        uniform_int_distribution<> numOpsDistr(1, 5);
        uniform_int_distribution<> feeDistr(100, 1000);
        uniform_int_distribution<> addFeeDistr(0, 5);
        uniform_int_distribution<> txCountDistr(1, 30);

        auto root = TestAccount::createRoot(*app);

        int nextAccId = 1;

        auto genTx = [&]() {
            auto account = root.create(std::to_string(nextAccId), 5000000000);
            ++nextAccId;
            int ops = numOpsDistr(Catch::rng());
            int fee = ops * feeDistr(Catch::rng()) + addFeeDistr(Catch::rng());
            if (isDexTxDistr(Catch::rng()) < dexOpsPercent)
            {
                return createSimpleDexTx(*app, account, ops, fee);
            }
            else
            {
                return makeSelfPayment(account, ops, fee);
            }
        };
        auto genTxs = [&](int cnt) {
            std::vector<TransactionFrameBasePtr> txs;
            for (int i = 0; i < cnt; ++i)
            {
                txs.emplace_back(genTx());
            }
            return txs;
        };

        for (int iter = 0; iter < 50; ++iter)
        {
            auto txs = genTxs(txCountDistr(Catch::rng()));
            auto txSet = TxSetFrame::makeFromTransactions(txs, *app, 0, 0);

            auto resTxs = txSet->getTxsInApplyOrder();
            std::array<uint32_t, 2> opsCounts{};
            std::array<int64_t, 2> baseFees{};
            for (auto const& resTx : resTxs)
            {
                auto isDex = static_cast<size_t>(resTx->hasDexOperations());
                opsCounts[isDex] += resTx->getNumOperations();
                auto baseFee = txSet->getTxBaseFee(resTx, lhCopy);
                REQUIRE(baseFee);
                if (baseFees[isDex] != 0)
                {
                    // All base fees should be the same among the transaction
                    // categories.
                    REQUIRE(baseFees[isDex] == *baseFee);
                }
                else
                {
                    baseFees[isDex] = *baseFee;
                }
            }
            REQUIRE(opsCounts[0] + opsCounts[1] <=
                    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
            if (maxDexOps)
            {
                REQUIRE(opsCounts[1] <= *maxDexOps);
            }
            // DEX transaction base fee has to be not smaller than generic
            // transaction base fee.
            if (baseFees[0] > 0 && baseFees[1] > 0)
            {
                REQUIRE(baseFees[0] <= baseFees[1]);
            }
        }
    };

    SECTION("no DEX limit")
    {
        runTest(std::nullopt, 50);
    }
    SECTION("low DEX limit")
    {
        SECTION("medium DEX tx fraction")
        {
            runTest(5, 50);
        }
        SECTION("high DEX tx fraction")
        {
            runTest(5, 80);
        }
        SECTION("only DEX txs")
        {
            runTest(5, 100);
        }
    }
    SECTION("high DEX limit")
    {
        SECTION("medium DEX tx fraction")
        {
            runTest(15, 50);
        }
        SECTION("high DEX tx fraction")
        {
            runTest(15, 80);
        }
        SECTION("only DEX txs")
        {
            runTest(15, 100);
        }
    }
}

TEST_CASE("generalized tx set applied to ledger", "[herder][txset]")
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    int64 startingBalance =
        app->getLedgerManager().getLastMinBalance(0) + 10000000;

    std::vector<TestAccount> accounts;
    int txCnt = 0;
    auto addTx = [&](int nbOps, uint32_t fee) {
        auto account = root.create(std::to_string(txCnt++), startingBalance);
        accounts.push_back(account);
        return makeSelfPayment(account, nbOps, fee);
    };

    auto checkFees = [&](TxSetFrameConstPtr txSet,
                         std::vector<int64_t> const& expectedFeeCharged) {
        REQUIRE(txSet->checkValid(*app, 0, 0));

        auto getBalances = [&]() {
            std::vector<int64_t> balances;
            std::transform(accounts.begin(), accounts.end(),
                           std::back_inserter(balances),
                           [](TestAccount& a) { return a.getBalance(); });
            return balances;
        };
        auto balancesBefore = getBalances();

        closeLedgerOn(*app,
                      app->getLedgerManager().getLastClosedLedgerNum() + 1,
                      getTestDate(13, 4, 2022), txSet);

        auto balancesAfter = getBalances();
        std::vector<int64_t> feeCharged;
        for (size_t i = 0; i < balancesAfter.size(); i++)
        {
            feeCharged.push_back(balancesBefore[i] - balancesAfter[i]);
        }

        REQUIRE(feeCharged == expectedFeeCharged);
    };

    SECTION("single discounted component")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(
                1000, std::vector<TransactionFrameBasePtr>{addTx(3, 3500),
                                                           addTx(2, 5000)})},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);
        checkFees(txSet, {3000, 2000});
    }
    SECTION("single non-discounted component")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(std::nullopt,
                            std::vector<TransactionFrameBasePtr>{
                                addTx(3, 3500), addTx(2, 5000)})},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);
        checkFees(txSet, {3500, 5000});
    }
    SECTION("multiple components")
    {
        std::vector<std::pair<std::optional<int64_t>,
                              std::vector<TransactionFrameBasePtr>>>
            components = {
                std::make_pair(
                    1000, std::vector<TransactionFrameBasePtr>{addTx(3, 3500),
                                                               addTx(2, 5000)}),
                std::make_pair(
                    500, std::vector<TransactionFrameBasePtr>{addTx(1, 501),
                                                              addTx(5, 10000)}),
                std::make_pair(2000,
                               std::vector<TransactionFrameBasePtr>{
                                   addTx(4, 15000),
                               }),
                std::make_pair(std::nullopt,
                               std::vector<TransactionFrameBasePtr>{
                                   addTx(5, 35000), addTx(1, 10000)})};
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            components, app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);
        checkFees(txSet, {3000, 2000, 500, 2500, 8000, 35000, 10000});
    }
}

static void
testSCPDriver(uint32 protocolVersion, uint32_t maxTxSetSize, size_t expectedOps)
{
    using SVUpgrades = decltype(StellarValue::upgrades);

    Config cfg(getTestConfig(0, Config::TESTDB_DEFAULT));

    cfg.MANUAL_CLOSE = false;
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = maxTxSetSize;

    VirtualClock clock;
    auto s = SecretKey::pseudoRandomForTesting();
    cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());

    Application::pointer app = createTestApplication(clock, cfg);

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto root = TestAccount::createRoot(*app);
    auto a1 = TestAccount{*app, getAccount("A")};

    using TxPair = std::pair<Value, TxSetFrameConstPtr>;
    auto makeTxUpgradePair = [&](HerderImpl& herder, TxSetFrameConstPtr txSet,
                                 uint64_t closeTime,
                                 SVUpgrades const& upgrades) {
        StellarValue sv = herder.makeStellarValue(
            txSet->getContentsHash(), closeTime, upgrades, root.getSecretKey());
        auto v = xdr::xdr_to_opaque(sv);
        return TxPair{v, txSet};
    };
    auto makeTxPair = [&](HerderImpl& herder, TxSetFrameConstPtr txSet,
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
    auto makeTransactions = [&](int n, int nbOps, uint32 feeMulti) {
        root.loadSequenceNumber();
        std::vector<TransactionFrameBasePtr> txs(n);
        std::generate(std::begin(txs), std::end(txs), [&]() {
            return makeMultiPayment(root, root, nbOps, 1000, 0, feeMulti);
        });

        return TxSetFrame::makeFromTransactions(txs, *app, 0, 0);
    };

    SECTION("combineCandidates")
    {
        auto& herder = static_cast<HerderImpl&>(app->getHerder());

        ValueWrapperPtrSet candidates;

        auto addToCandidates = [&](TxPair const& p) {
            auto envelope = makeEnvelope(
                herder, p, {}, herder.trackingConsensusLedgerIndex() + 1, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p.second->getContentsHash(), p.second));
            auto v = herder.getHerderSCPDriver().wrapValue(p.first);
            candidates.emplace(v);
        };

        struct CandidateSpec
        {
            int const n;
            int const nbOps;
            uint32 const feeMulti;
            TimePoint const closeTime;
            std::optional<uint32> const baseFeeIncrement;
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
            TxSetFrameConstPtr txSet =
                makeTransactions(spec.n, spec.nbOps, spec.feeMulti);
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
            {0, 1, 100, 10, std::nullopt},
            {10, 1, 100, 5, std::make_optional<uint32>(1)},
            {5, 3, 100, 20, std::make_optional<uint32>(2)},
            {7, 2, 5, 30, std::make_optional<uint32>(3)}};

        std::for_each(specs.begin(), specs.end(), addCandidateThenTest);

        auto const bestTxSetIndex = std::distance(
            txSetSizes.begin(),
            std::max_element(txSetSizes.begin(), txSetSizes.end()));
        REQUIRE(txSetOpSizes[bestTxSetIndex] == expectedOps);

        TxSetFrameConstPtr txSetL = makeTransactions(maxTxSetSize, 1, 101);
        addToCandidates(makeTxPair(herder, txSetL, 20));
        TxSetFrameConstPtr txSetL2 = makeTransactions(maxTxSetSize, 1, 1000);
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
        auto seq = herder.trackingConsensusLedgerIndex() + 1;
        auto ct = app->timeNow() + 1;

        TxSetFrameConstPtr txSet0 = makeTransactions(0, 1, 100);
        {
            // make sure that txSet0 is loaded
            auto p = makeTxPair(herder, txSet0, ct);
            auto envelope = makeEnvelope(herder, p, {}, seq, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(txSet0->getContentsHash(), txSet0));
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
            // Build a transaction set containing one transaction (which
            // could be any transaction that is valid in all ways aside from
            // its time bounds) with the given minTime and maxTime.
            auto tx = makeMultiPayment(root, root, 10, 1000, 0, 100);
            setMinTime(tx, minTime);
            setMaxTime(tx, maxTime);
            auto& sig = tx->getEnvelope().type() == ENVELOPE_TYPE_TX_V0
                            ? tx->getEnvelope().v0().signatures
                            : tx->getEnvelope().v1().signatures;
            sig.clear();
            tx->addSignature(root.getSecretKey());
            auto txSet = testtxset::makeNonValidatedTxSetBasedOnLedgerVersion(
                protocolVersion, {tx}, app->getNetworkID(),
                app->getLedgerManager().getLastClosedLedgerHeader().hash);

            // Build a StellarValue containing the transaction set we just
            // built and the given next closeTime.
            auto val = makeTxPair(herder, txSet, nextCloseTime);
            auto const seq = herder.trackingConsensusLedgerIndex() + 1;
            auto envelope = makeEnvelope(herder, val, {}, seq, true);
            REQUIRE(herder.recvSCPEnvelope(envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(txSet->getContentsHash(), txSet));

            // Validate the StellarValue.
            REQUIRE(scp.validateValue(seq, val.first, true) ==
                    (expectValid ? SCPDriver::kFullyValidatedValue
                                 : SCPDriver::kInvalidValue));

            // Confirm that getTxTrimList() as used by
            // TxSetFrame::makeFromTransactions() trims the transaction if and
            // only if we expect it to be invalid.
            auto closeTimeOffset = nextCloseTime - lclCloseTime;
            TxSetFrame::Transactions removed;
            TxSetUtils::trimInvalid(txSet->getTxs(), *app, closeTimeOffset,
                                    closeTimeOffset, removed);
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
        auto transactions1 = makeTransactions(5, 1, 100);
        auto transactions2 = makeTransactions(4, 1, 100);

        auto p1 = makeTxPair(herder, transactions1, 10);
        auto p2 = makeTxPair(herder, transactions1, 10);
        // use current + 1 to allow for any value (old values get filtered more)
        auto lseq = herder.trackingConsensusLedgerIndex() + 1;
        auto saneEnvelopeQ1T1 =
            makeEnvelope(herder, p1, saneQSet1Hash, lseq, true);
        auto saneEnvelopeQ1T2 =
            makeEnvelope(herder, p2, saneQSet1Hash, lseq, true);
        auto saneEnvelopeQ2T1 =
            makeEnvelope(herder, p1, saneQSet2Hash, lseq, true);
        auto bigEnvelope = makeEnvelope(herder, p1, bigQSetHash, lseq, true);

        TxSetFrameConstPtr malformedTxSet;
        if (transactions1->isGeneralizedTxSet())
        {
            GeneralizedTransactionSet xdrTxSet;
            transactions1->toXDR(xdrTxSet);
            auto& txs = xdrTxSet.v1TxSet()
                            .phases[0]
                            .v0Components()[0]
                            .txsMaybeDiscountedFee()
                            .txs;
            std::swap(txs[0], txs[1]);
            malformedTxSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
        }
        else
        {
            TransactionSet xdrTxSet;
            transactions1->toXDR(xdrTxSet);
            auto& txs = xdrTxSet.txs;
            std::swap(txs[0], txs[1]);
            malformedTxSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
        }
        auto malformedTxSetPair = makeTxPair(herder, malformedTxSet, 10);
        auto malformedTxSetEnvelope =
            makeEnvelope(herder, malformedTxSetPair, saneQSet1Hash, lseq, true);

        SECTION("return FETCHING until fetched")
        {
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvSCPQuorumSet(saneQSet1Hash, saneQSet1));
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));
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
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));

            SECTION("when re-receiving the same envelope")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ1T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(
                    !herder.recvTxSet(p1.second->getContentsHash(), p1.second));
            }

            SECTION("when receiving different envelope with the same txset")
            {
                REQUIRE(herder.recvSCPEnvelope(saneEnvelopeQ2T1) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(
                    !herder.recvTxSet(p1.second->getContentsHash(), p1.second));
            }

            SECTION("when receiving envelope with malformed tx set")
            {
                REQUIRE(herder.recvSCPEnvelope(malformedTxSetEnvelope) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(herder.recvTxSet(
                    malformedTxSetPair.second->getContentsHash(),
                    malformedTxSetPair.second));

                REQUIRE(herder.recvSCPEnvelope(malformedTxSetEnvelope) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(!herder.recvTxSet(
                    malformedTxSetPair.second->getContentsHash(),
                    malformedTxSetPair.second));
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
            REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(), p1.second));
            REQUIRE(!herder.recvTxSet(p2.second->getContentsHash(), p2.second));
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
            REQUIRE(!herder.recvTxSet(p1.second->getContentsHash(), p1.second));
        }

        SECTION(
            "accept txset from envelope with unsane qset before receiving qset")
        {
            REQUIRE(herder.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));
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
            REQUIRE(herder.recvTxSet(p1.second->getContentsHash(), p1.second));
        }

        SECTION("accept malformed txset, but fail validation")
        {
            REQUIRE(herder.recvSCPEnvelope(malformedTxSetEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(
                herder.recvTxSet(malformedTxSetPair.second->getContentsHash(),
                                 malformedTxSetPair.second));
            REQUIRE(herder.getHerderSCPDriver().validateValue(
                        herder.trackingConsensusLedgerIndex() + 1,
                        malformedTxSetPair.first,
                        false) == SCPDriver::kInvalidValue);
        }
    }
}

TEST_CASE("SCP Driver", "[herder][acceptance]")
{
    SECTION("before generalized tx set protocol")
    {
        testSCPDriver(static_cast<uint32>(GENERALIZED_TX_SET_PROTOCOL_VERSION) -
                          1,
                      1000, 15);
    }
    SECTION("generalized tx set protocol")
    {
        testSCPDriver(static_cast<uint32>(GENERALIZED_TX_SET_PROTOCOL_VERSION),
                      1000, 15);
    }
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
    // be externalized when first or second node are chosen as round leaders.
    // It some cases it can take more time than expected. Probability of that
    // is pretty low, but high enough that it forced us to rerun tests from
    // time to time to pass that one case.
    //
    // After changing node ids generated here from random to deterministics
    // this problem goes away, as the leader selection protocol uses node id
    // and round id for selecting leader.
    auto configure = [&](Config::TestDbMode mode) {
        for (int i = 0; i < 3; i++)
        {
            nodeKeys[i] =
                SecretKey::fromSeed(sha256("Node_" + std::to_string(i)));
            nodeIDs[i] = nodeKeys[i].getPublicKey();
            nodeCfgs[i] = getTestConfig(i + 1, mode);
        }
    };

    LedgerHeaderHistoryEntry lcl;
    uint32_t numLedgers = 5;
    uint32_t expectedLedger = LedgerManager::GENESIS_LEDGER_SEQ + numLedgers;
    std::unordered_set<Hash> knownTxSetHashes;

    auto checkTxSetHashesPersisted =
        [&](Application::pointer app,
            std::optional<
                std::unordered_map<uint32_t, std::vector<SCPEnvelope>>>
                expectedSCPState) {
            // Check that node0 restored state correctly
            auto& herder = static_cast<HerderImpl&>(app->getHerder());
            auto limit = app->getHerder().getMinLedgerSeqToRemember();

            std::unordered_set<Hash> hashes;
            for (auto i = app->getHerder().trackingConsensusLedgerIndex();
                 i >= limit; --i)
            {
                if (i == LedgerManager::GENESIS_LEDGER_SEQ)
                {
                    continue;
                }
                auto msgs = herder.getSCP().getLatestMessagesSend(i);
                if (expectedSCPState.has_value())
                {
                    auto state = *expectedSCPState;
                    REQUIRE(state.find(i) != state.end());
                    REQUIRE(msgs == state[i]);
                }
                for (auto const& msg : msgs)
                {
                    for (auto const& h : getTxSetHashes(msg))
                    {
                        REQUIRE(herder.getPendingEnvelopes().getTxSet(h));
                        REQUIRE(app->getPersistentState().hasTxSet(h));
                        hashes.insert(h);
                    }
                }
            }

            return hashes;
        };

    auto doTest = [&](bool forceSCP) {
        SECTION("sqlite")
        {
            configure(Config::TestDbMode::TESTDB_ON_DISK_SQLITE);
        }
#ifdef USE_POSTGRES
        SECTION("postgres")
        {
            configure(Config::TestDbMode::TESTDB_POSTGRESQL);
        }
#endif
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

        std::unordered_map<uint32_t, std::vector<SCPEnvelope>> nodeSCPState;
        auto lclNum = sim->getNode(nodeIDs[0])
                          ->getHerder()
                          .trackingConsensusLedgerIndex();
        // Save node's state before restart
        auto limit =
            sim->getNode(nodeIDs[0])->getHerder().getMinLedgerSeqToRemember();
        {
            auto& herder =
                static_cast<HerderImpl&>(sim->getNode(nodeIDs[0])->getHerder());
            for (auto i = lclNum; i > limit; --i)
            {
                nodeSCPState[i] = herder.getSCP().getLatestMessagesSend(i);
            }
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
        // 2 always has FORCE_SCP=true, so it starts in sync
        REQUIRE(sim->getNode(nodeIDs[2])->getState() ==
                Application::State::APP_SYNCED_STATE);

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

        // Check that node0 restored state correctly
        knownTxSetHashes =
            checkTxSetHashesPersisted(sim->getNode(nodeIDs[0]), nodeSCPState);

        if (forceSCP)
        {
            REQUIRE(sim->getNode(nodeIDs[0])->getState() ==
                    Application::State::APP_SYNCED_STATE);
            REQUIRE(sim->getNode(nodeIDs[1])->getState() ==
                    Application::State::APP_SYNCED_STATE);
        }
        else
        {
            REQUIRE(sim->getNode(nodeIDs[0])->getState() ==
                    Application::State::APP_CONNECTED_STANDBY_STATE);
            REQUIRE(sim->getNode(nodeIDs[1])->getState() ==
                    Application::State::APP_CONNECTED_STANDBY_STATE);
        }

        sim->addConnection(nodeIDs[0], nodeIDs[2]);
        sim->addConnection(nodeIDs[1], nodeIDs[2]);
        sim->addConnection(nodeIDs[0], nodeIDs[1]);
    };

    SECTION("Force SCP")
    {
        doTest(true);

        // then let the nodes run a bit more, they should all externalize the
        // next ledger
        sim->crankUntil(
            [&]() { return sim->haveAllExternalized(expectedLedger + 1, 5); },
            2 * numLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        // nodes are at least on ledger 7 (some may be on 8)
        for (int i = 0; i <= 2; i++)
        {
            // All nodes are in sync
            REQUIRE(sim->getNode(nodeIDs[i])->getState() ==
                    Application::State::APP_SYNCED_STATE);
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

        REQUIRE(sim->getNode(nodeIDs[0])->getState() ==
                Application::State::APP_CONNECTED_STANDBY_STATE);
        REQUIRE(sim->getNode(nodeIDs[1])->getState() ==
                Application::State::APP_CONNECTED_STANDBY_STATE);
        REQUIRE(sim->getNode(nodeIDs[2])->getState() ==
                Application::State::APP_SYNCED_STATE);

        for (int i = 0; i <= 2; i++)
        {
            auto const& actual = sim->getNode(nodeIDs[i])
                                     ->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header;
            REQUIRE(actual == lcl.header);
        }

        // Crank some more and let 2 go out of sync
        sim->crankUntil(
            [&]() {
                return sim->getNode(nodeIDs[2])->getHerder().getState() ==
                       Herder::State::HERDER_SYNCING_STATE;
            },
            10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        // Verify that the app is not synced anymore
        REQUIRE(sim->getNode(nodeIDs[2])->getState() ==
                Application::State::APP_ACQUIRING_CONSENSUS_STATE);
    }
    SECTION("SCP State Persistence")
    {
        doTest(true);
        // Remove last node so node0 and node1 are guaranteed to end up at
        // `expectedLedger + MAX_SLOTS_TO_REMEMBER + 1`
        sim->removeNode(nodeIDs[2]);
        // Crank for MAX_SLOTS_TO_REMEMBER + 1, so that purging logic kicks in
        sim->crankUntil(
            [&]() {
                // One extra ledger because tx sets are purged whenever new slot
                // is started
                return sim->haveAllExternalized(
                    expectedLedger + nodeCfgs[0].MAX_SLOTS_TO_REMEMBER + 1, 1);
            },
            2 * nodeCfgs[0].MAX_SLOTS_TO_REMEMBER *
                Herder::EXP_LEDGER_TIMESPAN_SECONDS,
            false);

        // Remove node1 so node0 can't make progress
        sim->removeNode(nodeIDs[1]);
        // Crank until tx set GC kick in
        sim->crankForAtLeast(Herder::TX_SET_GC_DELAY * 2, false);

        // First,  check that node removed all persisted state for ledgers <=
        // expectedLedger
        auto app = sim->getNode(nodeIDs[0]);

        for (auto const& txSetHash : knownTxSetHashes)
        {
            REQUIRE(!app->getPersistentState().hasTxSet(txSetHash));
        }

        // Now, ensure all new tx sets have been persisted
        checkTxSetHashesPersisted(app, std::nullopt);
    }
}

static void
checkSynced(Application& app)
{
    REQUIRE(app.getLedgerManager().isSynced());
    REQUIRE(!app.getCatchupManager().maybeGetNextBufferedLedgerToApply());
}

void
checkInvariants(Application& app, HerderImpl& herder)
{
    auto lcl = app.getLedgerManager().getLastClosedLedgerNum();
    // Either tracking or last tracking must be set
    // Tracking is ahead of or equal to LCL
    REQUIRE(herder.trackingConsensusLedgerIndex() >= lcl);
}

static void
checkHerder(Application& app, HerderImpl& herder, Herder::State expectedState,
            uint32_t ledger)
{
    checkInvariants(app, herder);
    REQUIRE(herder.getState() == expectedState);
    REQUIRE(herder.trackingConsensusLedgerIndex() == ledger);
}

// The main purpose of this test is to ensure the externalize path works
// correctly. This entails properly updating tracking in Herder, forwarding
// externalize information to LM, and Herder appropriately reacting to ledger
// close.

// The nice thing about this test is that because we fully control the messages
// received by a node, we fully control the state of Herder and LM (and whether
// each component is in sync or out of sync)
TEST_CASE("herder externalizes values", "[herder]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = std::make_shared<Simulation>(
        Simulation::OVER_LOOPBACK, networkID,
        [](int i) { return getTestConfig(i, Config::TESTDB_ON_DISK_SQLITE); });

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

    auto getC = [&]() {
        return simulation->getNode(validatorCKey.getPublicKey());
    };

    // Before application is started, Herder is booting
    REQUIRE(getC()->getHerder().getState() ==
            Herder::State::HERDER_BOOTING_STATE);

    simulation->startAllNodes();

    // After SCP is restored, Herder is tracking
    REQUIRE(getC()->getHerder().getState() ==
            Herder::State::HERDER_TRACKING_NETWORK_STATE);

    auto A = simulation->getNode(validatorAKey.getPublicKey());
    auto B = simulation->getNode(validatorBKey.getPublicKey());

    auto currentALedger = [&]() {
        return A->getLedgerManager().getLastClosedLedgerNum();
    };
    auto currentBLedger = [&]() {
        return B->getLedgerManager().getLastClosedLedgerNum();
    };
    auto currentCLedger = [&]() {
        return getC()->getLedgerManager().getLastClosedLedgerNum();
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

    auto waitForAB = [&](int nLedgers, bool waitForB) {
        auto destinationLedger = currentALedger() + nLedgers;
        simulation->crankUntil(
            [&]() {
                return currentALedger() >= destinationLedger &&
                       (!waitForB || currentBLedger() >= destinationLedger);
            },
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
    HerderImpl& herderC = *static_cast<HerderImpl*>(&getC()->getHerder());
    auto const& lmC = getC()->getLedgerManager();

    // Advance A and B a bit further, and collect externalize messages
    std::map<uint32_t, std::pair<SCPEnvelope, TxSetFrameConstPtr>>
        validatorSCPMessagesA;
    std::map<uint32_t, std::pair<SCPEnvelope, TxSetFrameConstPtr>>
        validatorSCPMessagesB;

    auto destinationLedger = waitForAB(4, true);
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
    checkHerder(*(getC()), herderC,
                Herder::State::HERDER_TRACKING_NETWORK_STATE, currentCLedger());
    REQUIRE(currentCLedger() == currentLedger);

    auto receiveLedger = [&](uint32_t ledger, Herder& herder) {
        auto newMsgB = validatorSCPMessagesB.at(ledger);
        auto newMsgA = validatorSCPMessagesA.at(ledger);

        REQUIRE(herder.recvSCPEnvelope(newMsgA.first, qset, newMsgA.second) ==
                Herder::ENVELOPE_STATUS_READY);
        REQUIRE(herder.recvSCPEnvelope(newMsgB.first, qset, newMsgB.second) ==
                Herder::ENVELOPE_STATUS_READY);
    };

    auto testOutOfOrder = [&](bool partial) {
        auto first = currentLedger + 1;
        auto second = first + 1;
        auto third = second + 1;
        auto fourth = third + 1;

        // Drop A-B connection, so that the network can't make progress
        REQUIRE(currentALedger() == fourth);
        simulation->dropConnection(validatorAKey.getPublicKey(),
                                   validatorBKey.getPublicKey());

        // Externalize future ledger
        // This should trigger CatchupManager to start buffering ledgers
        receiveLedger(fourth, herderC);

        // Wait until C goes out of sync, and processes future slots
        simulation->crankUntil([&]() { return !lmC.isSynced(); },
                               2 * Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS,
                               false);

        // Ensure LM is out of sync, and Herder tracks ledger seq from latest
        // envelope
        REQUIRE(!lmC.isSynced());
        checkHerder(*(getC()), herderC,
                    Herder::State::HERDER_TRACKING_NETWORK_STATE, fourth);
        REQUIRE(herderC.getTriggerTimer().seq() == 0);

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
            receiveLedger(ledgers[i], herderC);

            // Tracking did not change
            checkHerder(*(getC()), herderC,
                        Herder::State::HERDER_TRACKING_NETWORK_STATE, fourth);
            REQUIRE(!getC()->getCatchupManager().isCatchupInitialized());

            // At the last ledger, LM is back in sync
            if (i == ledgers.size() - 1)
            {
                checkSynced(*(getC()));
                // All the buffered ledgers are applied by now, so it's safe to
                // trigger the next ledger
                REQUIRE(herderC.getTriggerTimer().seq() > 0);
                REQUIRE(herderC.mTriggerNextLedgerSeq == fourth + 1);
            }
            else
            {
                REQUIRE(!lmC.isSynced());
                // As we're not in sync yet, ensure next ledger is not triggered
                REQUIRE(herderC.getTriggerTimer().seq() == 0);
                REQUIRE(herderC.mTriggerNextLedgerSeq == currentLedger + 1);
            }
        }

        // As we're back in sync now, ensure Herder and LM are consistent with
        // each other
        auto lcl = lmC.getLastClosedLedgerNum();
        REQUIRE(lcl == herderC.trackingConsensusLedgerIndex());

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

    SECTION("newer ledgers externalize in order")
    {
        auto checkReceivedLedgers = [&]() {
            for (auto const& msgPair : validatorSCPMessagesA)
            {
                receiveLedger(msgPair.first, herderC);

                // Tracking is updated correctly
                checkHerder(*(getC()), herderC,
                            Herder::State::HERDER_TRACKING_NETWORK_STATE,
                            msgPair.first);
                // LM is synced
                checkSynced(*(getC()));

                // Since we're externalizing ledgers in order, make sure ledger
                // trigger is scheduled
                REQUIRE(herderC.getTriggerTimer().seq() > 0);
                REQUIRE(herderC.mTriggerNextLedgerSeq == msgPair.first + 1);
            }
        };

        SECTION("tracking")
        {
            checkHerder(*(getC()), herderC,
                        Herder::State::HERDER_TRACKING_NETWORK_STATE,
                        currentLedger);
            checkReceivedLedgers();
        }
        SECTION("not tracking")
        {
            simulation->crankUntil(
                [&]() {
                    return herderC.getState() ==
                           Herder::State::HERDER_SYNCING_STATE;
                },
                2 * Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS, false);
            checkHerder(*(getC()), herderC, Herder::State::HERDER_SYNCING_STATE,
                        currentLedger);
            checkReceivedLedgers();
        }
    }
    SECTION("newer ledgers externalize out of order")
    {
        SECTION("completely")
        {
            testOutOfOrder(/* partial */ false);
        }
        SECTION("partial")
        {
            testOutOfOrder(/* partial */ true);
        }
    }

    SECTION("older ledgers externalize and no-op")
    {
        // Reconnect nodes to crank the simulation just enough to purge older
        // slots
        auto configC = getC()->getConfig();
        simulation->addConnection(validatorAKey.getPublicKey(),
                                  validatorBKey.getPublicKey());
        simulation->addConnection(validatorAKey.getPublicKey(),
                                  validatorCKey.getPublicKey());
        auto currentlyTracking =
            waitForLedgers(configC.MAX_SLOTS_TO_REMEMBER + 1);

        // Restart C with higher MAX_SLOTS_TO_REMEMBER config, to allow
        // processing of older slots
        simulation->removeNode(validatorCKey.getPublicKey());
        configC.MAX_SLOTS_TO_REMEMBER += 5;
        auto newC = simulation->addNode(validatorCKey, qset, &configC, false);
        newC->start();
        HerderImpl& newHerderC = *static_cast<HerderImpl*>(&newC->getHerder());

        checkHerder(*newC, newHerderC,
                    Herder::State::HERDER_TRACKING_NETWORK_STATE,
                    currentlyTracking);

        SECTION("tracking")
        {
            receiveLedger(destinationLedger, newHerderC);

            checkHerder(*newC, newHerderC,
                        Herder::State::HERDER_TRACKING_NETWORK_STATE,
                        currentlyTracking);
            checkSynced(*newC);
            // Externalizing an old ledger should not trigger next ledger
            REQUIRE(newHerderC.mTriggerNextLedgerSeq == currentlyTracking + 1);
        }
        SECTION("not tracking")
        {
            // Wait until C goes out of sync
            simulation->crankUntil(
                [&]() {
                    return newHerderC.getState() ==
                           Herder::State::HERDER_SYNCING_STATE;
                },
                2 * Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS, false);
            checkHerder(*newC, newHerderC, Herder::State::HERDER_SYNCING_STATE,
                        currentlyTracking);

            receiveLedger(destinationLedger, newHerderC);

            // Tracking has not changed, still the most recent ledger
            checkHerder(*newC, newHerderC, Herder::State::HERDER_SYNCING_STATE,
                        currentlyTracking);
            checkSynced(*newC);

            // Externalizing an old ledger should not trigger next ledger
            REQUIRE(newHerderC.mTriggerNextLedgerSeq == currentlyTracking + 1);
        }
    }
    SECTION("trigger next ledger")
    {
        // Sync C with the rest of the network
        testOutOfOrder(/* partial */ false);

        // Reconnect C to the rest of the network
        simulation->addConnection(validatorAKey.getPublicKey(),
                                  validatorCKey.getPublicKey());
        SECTION("C goes back in sync and unsticks the network")
        {
            // Now that C is back in sync and triggered next ledger
            // (and B is disconnected), C and A should be able to make progress

            auto lcl = currentALedger();
            auto nextLedger = lcl + fewLedgers;

            // Make sure A and C are starting from the same ledger
            REQUIRE(lcl == currentCLedger());

            waitForAB(fewLedgers, false);
            REQUIRE(currentALedger() == nextLedger);
            // C is at most a ledger behind
            REQUIRE(currentCLedger() >= nextLedger - 1);
        }
        SECTION("restarting C should not trigger twice")
        {
            auto configC = getC()->getConfig();

            simulation->removeNode(validatorCKey.getPublicKey());

            auto newC =
                simulation->addNode(validatorCKey, qset, &configC, false);

            // Restarting C should trigger due to FORCE_SCP
            newC->start();
            HerderImpl& newHerderC =
                *static_cast<HerderImpl*>(&newC->getHerder());

            auto expiryTime = newHerderC.getTriggerTimer().expiry_time();
            REQUIRE(newHerderC.getTriggerTimer().seq() > 0);

            simulation->crankForAtLeast(std::chrono::seconds(1), false);

            // C receives enough messages to externalize LCL again
            receiveLedger(newC->getLedgerManager().getLastClosedLedgerNum(),
                          newHerderC);

            // Trigger timer did not change
            REQUIRE(expiryTime == newHerderC.getTriggerTimer().expiry_time());
            REQUIRE(newHerderC.getTriggerTimer().seq() > 0);
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
            std::vector<TransactionFrameBasePtr> const& txs, Application& app)
{
    auto const& lcl = lm.getLastClosedLedgerHeader();
    auto ledgerSeq = lcl.header.ledgerSeq + 1;

    auto txSet = TxSetFrame::makeFromTransactions(txs, app, 0, 0);
    herder.getPendingEnvelopes().putTxSet(txSet->getContentsHash(), ledgerSeq,
                                          txSet);

    auto lastCloseTime = lcl.header.scpValue.closeTime;

    StellarValue sv =
        herder.makeStellarValue(txSet->getContentsHash(), lastCloseTime,
                                xdr::xvector<UpgradeType, 6>{}, sk);
    herder.getHerderSCPDriver().valueExternalized(ledgerSeq,
                                                  xdr::xdr_to_opaque(sv));
}

TEST_CASE("do not flood invalid transactions", "[herder]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FLOOD_TX_PERIOD_MS = 1; // flood as fast as possible
    auto app = createTestApplication(clock, cfg);

    auto& lm = app->getLedgerManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    auto& tq = herder.getTransactionQueue();

    auto root = TestAccount::createRoot(*app);
    auto acc = root.create("A", lm.getLastMinBalance(2));

    auto tx1a = acc.tx({payment(acc, 1)});
    auto tx1r = root.tx({bumpSequence(INT64_MAX)});
    // this will be invalid after tx1r gets applied
    auto tx2r = root.tx({payment(root, 1)});

    herder.recvTransaction(tx1a, false);
    herder.recvTransaction(tx1r, false);
    herder.recvTransaction(tx2r, false);

    size_t numBroadcast = 0;
    tq.mTxBroadcastedEvent = [&](TransactionFrameBasePtr&) { ++numBroadcast; };

    externalize(cfg.NODE_SEED, lm, herder, {tx1r}, *app);
    auto timeout = clock.now() + std::chrono::seconds(5);
    while (numBroadcast != 1)
    {
        clock.crank(true);
        REQUIRE(clock.now() < timeout);
    }

    auto const& lhhe = lm.getLastClosedLedgerHeader();
    auto txs = tq.getTransactions(lhhe.header);
    auto txSet = TxSetFrame::makeFromTransactions(txs, *app, 0, 0);
    REQUIRE(txSet->sizeTx() == 1);
    REQUIRE(txSet->getTxs().front()->getContentsHash() ==
            tx1a->getContentsHash());
    REQUIRE(txSet->checkValid(*app, 0, 0));
}

TEST_CASE("do not flood too many transactions", "[herder][transactionqueue]")
{
    auto test = [](uint32_t numOps) {
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto simulation = std::make_shared<Simulation>(
            Simulation::OVER_LOOPBACK, networkID, [&](int i) {
                auto cfg = getTestConfig(i);
                cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 500;
                cfg.NODE_IS_VALIDATOR = false;
                cfg.FORCE_SCP = false;
                cfg.FLOOD_TX_PERIOD_MS = 100;
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

            REQUIRE(herder.recvTransaction(tx, false) ==
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
        size_t numBroadcast = 0;
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
            ++numBroadcast;
        };

        REQUIRE(tq.getTransactions({}).size() == numTx);

        // remove the first two transactions that won't be
        // re-broadcasted during externalize
        fees.pop_front();
        fees.pop_front();

        externalize(cfg.NODE_SEED, lm, herder, {tx1a, tx1r}, *app);

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
        REQUIRE(tq.getTransactions({}).size() == numTx - 1);
        simulation->stopAllNodes();
    };

    SECTION("one operation per transaction")
    {
        test(1);
    }
    SECTION("a few operations per transaction")
    {
        test(7);
    }
    SECTION("full transactions")
    {
        test(100);
    }
}

TEST_CASE("do not flood too many transactions with DEX separation",
          "[herder][transactionqueue]")
{
    auto test = [](uint32_t dexTxs, uint32_t nonDexTxs, uint32_t opsPerDexTx,
                   uint32_t opsPerNonDexTx, bool broadcastDexFirst,
                   bool shuffleDexAndNonDex, int maxNoBroadcastPeriods) {
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto simulation = std::make_shared<Simulation>(
            Simulation::OVER_LOOPBACK, networkID, [&](int i) {
                auto cfg = getTestConfig(i);
                cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 500;
                cfg.NODE_IS_VALIDATOR = false;
                cfg.FORCE_SCP = false;
                cfg.FLOOD_TX_PERIOD_MS = 100;
                cfg.FLOOD_OP_RATE_PER_LEDGER = 2.0;
                cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET = 200;
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
        // number of transactions to generate per fee groups
        int const feeGroupMaxSize = 7;
        // used to track fee
        int feeGroupSize = 0;
        uint32_t curFeeOffset = 10000;

        accs.reserve(nbAccounts);
        UnorderedMap<AccountID, int> accountToIndex;
        for (int i = 0; i < nbAccounts; ++i)
        {
            auto accKey = getAccount(fmt::format("A{}", i));
            accs.emplace_back(root.create(accKey, lm.getLastMinBalance(2)));
            accountToIndex[accKey.getPublicKey()] = i;
        }
        std::vector<std::deque<std::pair<int64_t, bool>>> accountFees(
            nbAccounts);

        auto genTx = [&](size_t accountIndex, bool isDex, uint32_t numOps,
                         bool highFee) {
            std::vector<Operation> ops;
            auto& source = accs[accountIndex];
            if (isDex)
            {

                Asset asset1(ASSET_TYPE_CREDIT_ALPHANUM4);
                strToAssetCode(asset1.alphaNum4().assetCode, "USD");
                Asset asset2(ASSET_TYPE_NATIVE);
                for (uint32_t i = 1; i <= numOps; ++i)
                {
                    ops.emplace_back(
                        manageBuyOffer(i, asset1, asset2, Price{2, 5}, 10));
                }
            }
            else
            {
                for (uint32_t i = 1; i <= numOps; ++i)
                {
                    ops.emplace_back(payment(source, i));
                }
            }
            auto tx = source.tx(ops);
            auto txFee = tx->getFeeBid();
            if (highFee)
            {
                txFee += 100000;
                accountFees[accountIndex].emplace_front(txFee, isDex);
            }
            else
            {
                txFee += curFeeOffset;
                accountFees[accountIndex].emplace_back(txFee, isDex);
            }
            setFee(tx, txFee);
            getSignatures(tx).clear();
            tx->addSignature(source.getSecretKey());
            if (++feeGroupSize == feeGroupMaxSize)
            {
                feeGroupSize = 0;
                curFeeOffset--;
            }

            REQUIRE(herder.recvTransaction(tx, false) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            return tx;
        };

        auto genTxRandAccount = [&](bool isDex, uint32_t numOps) {
            genTx(autocheck::generator<size_t>()(nbAccounts - 3), isDex, numOps,
                  false);
        };

        // Reserve 1 tx in each non-empty group to add in the middle of the
        // ledger.
        if (dexTxs > 0)
        {
            --dexTxs;
        }
        if (nonDexTxs > 0)
        {
            --nonDexTxs;
        }
        if (shuffleDexAndNonDex)
        {
            auto boolGen = autocheck::generator<bool>();
            uint32_t generatedDex = 0, generatedNonDex = 0;
            while (generatedDex < dexTxs || generatedNonDex < nonDexTxs)
            {
                bool isDex = generatedDex < dexTxs &&
                             (generatedNonDex >= nonDexTxs || boolGen());
                if (isDex)
                {
                    genTxRandAccount(true, opsPerDexTx);
                    ++generatedDex;
                }
                else
                {
                    genTxRandAccount(false, opsPerNonDexTx);
                    ++generatedNonDex;
                }
            }
        }
        else
        {
            if (broadcastDexFirst)
            {
                for (uint32_t i = 0; i < dexTxs; ++i)
                {
                    genTxRandAccount(true, opsPerDexTx);
                }
            }
            for (uint32_t i = 0; i < nonDexTxs; ++i)
            {
                genTxRandAccount(false, opsPerNonDexTx);
            }
            if (!broadcastDexFirst)
            {
                for (uint32_t i = 0; i < dexTxs; ++i)
                {
                    genTxRandAccount(true, opsPerDexTx);
                }
            }
        }

        REQUIRE(tq.getTransactions({}).size() == dexTxs + nonDexTxs);

        std::map<AccountID, SequenceNumber> accountSeqNum;
        uint32_t dexOpsBroadcasted = 0;
        uint32_t nonDexOpsBroadcasted = 0;
        tq.mTxBroadcastedEvent = [&](TransactionFrameBasePtr& tx) {
            // Ensure that sequence numbers are correct per account.
            if (accountSeqNum.find(tx->getSourceID()) == accountSeqNum.end())
            {
                accountSeqNum[tx->getSourceID()] = tx->getSeqNum();
            }
            REQUIRE(accountSeqNum[tx->getSourceID()] == tx->getSeqNum());
            ++accountSeqNum[tx->getSourceID()];

            bool isDex = tx->hasDexOperations();
            // We expect the fee to be the highest among the accounts that
            // have the current transaction from the same group (i.e. DEX or
            // non-DEX).
            auto expectedFee =
                std::max_element(
                    accountFees.begin(), accountFees.end(),
                    [isDex](auto const& feesA, auto const& feesB) {
                        if (feesA.empty() || feesB.empty())
                        {
                            return !feesB.empty();
                        }
                        if (feesA.front().second != feesB.front().second)
                        {
                            return feesA.front().second != isDex;
                        }
                        return feesA.front().first < feesB.front().first;
                    })
                    ->front()
                    .first;

            REQUIRE(tx->getFeeBid() == expectedFee);
            accountFees[accountToIndex[tx->getSourceID()]].pop_front();
            if (tx->hasDexOperations())
            {
                dexOpsBroadcasted += tx->getNumOperations();
            }
            else
            {
                nonDexOpsBroadcasted += tx->getNumOperations();
            }
        };

        // no broadcast right away
        REQUIRE(dexOpsBroadcasted == 0);
        REQUIRE(nonDexOpsBroadcasted == 0);

        // wait for a bit more than a broadcast period
        // rate per period is
        // 2*(maxOps=500)*(FLOOD_TX_PERIOD_MS=100)/((ledger time=5)*1000)
        // 1000*100/5000=20
        auto constexpr opsRatePerPeriod = 20;
        auto constexpr dexOpsRatePerPeriod = 8;
        auto broadcastPeriod =
            std::chrono::milliseconds(cfg.FLOOD_TX_PERIOD_MS);
        auto const delta = std::chrono::milliseconds(1);
        int noBroadcastPeriods = 0;

        // Make 50(=5s/100ms) broadcast 'iterations' by cranking timer for
        // broadcastPeriod.
        for (int broadcastIter = 0; broadcastIter < 50; ++broadcastIter)
        {
            // Inject new transactions from unused account in the middle of
            // ledger period.
            if (broadcastIter == 25)
            {
                if (dexTxs > 0)
                {
                    ++dexTxs;
                    genTx(nbAccounts - 2, true, opsPerDexTx, true);
                }
                if (nonDexTxs > 0)
                {
                    ++nonDexTxs;
                    genTx(nbAccounts - 1, false, opsPerNonDexTx, true);
                }
            }
            auto lastDexOpsBroadcasted = dexOpsBroadcasted;
            auto lastNonDexOpsBroadcasted = nonDexOpsBroadcasted;
            simulation->crankForAtLeast(broadcastPeriod + delta, false);
            auto dexOpsPerPeriod = dexOpsBroadcasted - lastDexOpsBroadcasted;
            auto nonDexOpsPerPeriod =
                nonDexOpsBroadcasted - lastNonDexOpsBroadcasted;
            if (dexOpsPerPeriod + nonDexOpsBroadcasted == 0)
            {
                ++noBroadcastPeriods;
            }
            REQUIRE(dexOpsPerPeriod <= cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE + 1);
            REQUIRE(nonDexOpsPerPeriod <=
                    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE + 1);
            // We should broadcast the high fee transactions added at iteration
            // 25 within the number of periods according to the DEX/general
            // operation rates.
            if (dexTxs > 0 && broadcastIter ==
                                  25 + opsPerDexTx / dexOpsRatePerPeriod +
                                      opsPerDexTx % dexOpsRatePerPeriod !=
                                  0)
            {
                REQUIRE(accountFees[nbAccounts - 2].empty());
            }
            if (nonDexTxs > 0 &&
                broadcastIter ==
                    25 +
                        opsPerNonDexTx /
                            (opsRatePerPeriod - dexOpsRatePerPeriod) +
                        opsPerNonDexTx %
                            (opsRatePerPeriod - dexOpsRatePerPeriod) !=
                    0)
            {
                REQUIRE(accountFees[nbAccounts - 1].empty());
            }
        }

        REQUIRE(dexOpsBroadcasted == opsPerDexTx * dexTxs);
        REQUIRE(nonDexOpsBroadcasted == opsPerNonDexTx * nonDexTxs);
        // It's tricky to measure how closely do we follow the operations rate
        // due to existence of broadcast operations 'credit', so we just make
        // sure that the load is more or less even by looking at the upper bound
        // of idle periods (the more we have, the more we broadcast at too high
        // rate).
        REQUIRE(noBroadcastPeriods <= maxNoBroadcastPeriods);
        simulation->stopAllNodes();
    };

    SECTION("DEX-only, low ops")
    {
        test(400, 0, 1, 1, true, false, 0);
    }
    SECTION("DEX-only, med ops")
    {
        test(400 / 7, 0, 7, 1, true, false, 0);
    }
    SECTION("DEX-only, high ops")
    {
        // Broadcast only during 4 cycles.
        test(4, 0, 100, 1, true, false, 46);
    }

    SECTION("non-DEX-only, low ops")
    {
        test(0, 1000, 1, 1, true, false, 0);
    }
    SECTION("non-DEX-only, med ops")
    {
        test(0, 1000 / 7, 1, 7, true, false, 0);
    }
    SECTION("non-DEX-only, high ops")
    {
        // Broadcast only during 10 cycles.
        test(0, 10, 1, 100, true, false, 40);
    }

    SECTION("DEX before non-DEX, low ops")
    {
        test(300, 400, 1, 1, true, false, 0);
    }
    SECTION("DEX before non-DEX, med ops")
    {
        test(300 / 7, 400 / 7, 7, 7, true, false, 0);
    }
    SECTION("DEX before non-DEX, high ops")
    {
        test(300 / 100, 400 / 100, 100, 100, true, false, 43);
    }

    SECTION("DEX after non-DEX, low ops")
    {
        test(300, 400, 1, 1, false, false, 0);
    }
    SECTION("DEX after non-DEX, med ops")
    {
        test(300 / 7, 400 / 7, 7, 7, false, false, 0);
    }
    SECTION("DEX after non-DEX, high ops")
    {
        test(300 / 100, 400 / 100, 100, 100, false, false, 43);
    }

    SECTION("DEX shuffled with non-DEX, low ops")
    {
        test(300, 400, 1, 1, false, true, 0);
    }
    SECTION("DEX shuffled with non-DEX, med ops")
    {
        test(300 / 7, 400 / 7, 7, 7, false, true, 0);
    }
    SECTION("DEX shuffled with  non-DEX, high ops")
    {
        test(300 / 100, 400 / 100, 100, 100, false, true, 43);
    }

    SECTION("DEX shuffled with non-DEX, med DEX ops, high non-DEX")
    {
        test(300 / 9, 400 / 100, 9, 100, false, true, 5);
    }
    SECTION("DEX shuffled with non-DEX, high DEX ops, med non-DEX")
    {
        test(300 / 100, 400 / 9, 100, 9, false, true, 5);
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
    cfg.FORCE_SCP = false;
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
        TxSetFrameConstPtr txSet = TxSetFrame::makeEmpty(
            app->getLedgerManager().getLastClosedLedgerHeader());

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
        auto res = herder.recvSCPEnvelope(envelope, qSet, txSet);
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
    REQUIRE(herder.getState() == Herder::HERDER_TRACKING_NETWORK_STATE);
    REQUIRE(herder.getSCP().getKnownSlotsCount() == LIMIT);

    auto oneSec = std::chrono::seconds(1);
    // let the node go out of sync, it should reach the desired state
    timeout = clock.now() + Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS + oneSec;
    while (herder.getState() == Herder::HERDER_TRACKING_NETWORK_STATE)
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

        auto root = TestAccount::createRoot(*app);
        auto acc = getAccount("acc");
        auto tx = root.tx({createAccount(acc.getPublicKey(), 1)});

        REQUIRE(app->getHerder().recvTransaction(tx, false) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);
    }

    SECTION("filter excludes transaction containing specified operation")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE = {CREATE_ACCOUNT};
        Application::pointer app = createTestApplication(clock, cfg);

        auto root = TestAccount::createRoot(*app);
        auto acc = getAccount("acc");
        auto tx = root.tx({createAccount(acc.getPublicKey(), 1)});

        REQUIRE(app->getHerder().recvTransaction(tx, false) ==
                TransactionQueue::AddResult::ADD_STATUS_FILTERED);
    }

    SECTION("filter does not exclude transaction containing non-specified "
            "operation")
    {
        VirtualClock clock;
        auto cfg = getTestConfig();
        cfg.EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE = {MANAGE_DATA};
        Application::pointer app = createTestApplication(clock, cfg);

        auto root = TestAccount::createRoot(*app);
        auto acc = getAccount("acc");
        auto tx = root.tx({createAccount(acc.getPublicKey(), 1)});

        REQUIRE(app->getHerder().recvTransaction(tx, false) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);
    }
}
