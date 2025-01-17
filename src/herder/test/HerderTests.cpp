// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/test/TestTxSetUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/test.h"

#include "history/test/HistoryTestsUtils.h"

#include "catchup/LedgerApplyManagerImpl.h"
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
#include "transactions/test/TransactionTestFrame.h"
#include "util/Math.h"
#include "util/ProtocolVersion.h"

#include "crypto/Hex.h"
#include "ledger/test/LedgerTestUtils.h"
#include "test/TxTests.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/autocheck.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <memory>
#include <numeric>
#include <optional>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;
using namespace historytestutils;

TEST_CASE_VERSIONS("standalone", "[herder][acceptance]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());

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

            auto feedTx = [&](TransactionTestFramePtr tx,
                              TransactionQueue::AddResultCode expectedRes) {
                REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                        expectedRes);
            };

            auto waitForExternalize = [&]() {
                auto prev = app->getLedgerManager().getLastClosedLedgerNum();
                while (app->getLedgerManager().getLastClosedLedgerNum() <=
                       prev + 1)
                {
                    app->getClock().crank(true);
                }
            };

            auto setup = [&](asio::error_code const& error) {
                REQUIRE(!error);
                // create accounts
                auto txFrame = root.tx({createAccount(a1, startingBalance),
                                        createAccount(b1, startingBalance),
                                        createAccount(c1, startingBalance)});

                feedTx(txFrame,
                       TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
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
                bool hasC = false;
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    hasC = protocolVersionStartsFrom(
                        ltx.loadHeader().current().ledgerVersion,
                        ProtocolVersion::V_10);
                }

                std::vector<TransactionTestFramePtr> txAs, txBs, txCs;
                txAs.emplace_back(a1.tx({payment(root, paymentAmount)}));
                txAs.emplace_back(b1.tx({payment(root, paymentAmount)}));
                if (hasC)
                {
                    txAs.emplace_back(c1.tx({payment(root, paymentAmount)}));
                }

                for (auto a : txAs)
                {
                    feedTx(a,
                           TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                }
                waitForExternalize();

                txBs.emplace_back(a1.tx({payment(root, paymentAmount)}));
                txBs.emplace_back(b1.tx({accountMerge(root)}));
                auto expectedC1Seq = c1.getLastSequenceNumber() + 10;
                if (hasC)
                {
                    txBs.emplace_back(c1.tx({bumpSequence(expectedC1Seq)}));
                }

                for (auto b : txBs)
                {
                    feedTx(b,
                           TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                }
                waitForExternalize();

                txCs.emplace_back(a1.tx({payment(root, paymentAmount)}));
                txCs.emplace_back(b1.tx({payment(a1, paymentAmount)}));
                txCs.emplace_back(c1.tx({payment(root, paymentAmount)}));

                feedTx(txCs[0],
                       TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                feedTx(txCs[1],
                       TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                if (hasC)
                {
                    feedTx(txCs[2],
                           TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
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
                        startingBalance - paymentAmount - 2 * txfee;
                    REQUIRE(c1.getBalance() == expectedCBalance);
                    REQUIRE(c1.loadSequenceNumber() == expectedC1Seq);
                }
            }
        }
    });
}

static TransactionTestFramePtr
makeMultiPayment(stellar::TestAccount& destAccount, stellar::TestAccount& src,
                 int nbOps, int64 paymentBase, uint32 extraFee, uint32 feeMult)
{
    std::vector<stellar::Operation> ops;
    for (int i = 0; i < nbOps; i++)
    {
        ops.emplace_back(payment(destAccount, i + paymentBase));
    }
    auto tx = src.tx(ops);
    setFullFee(tx,
               static_cast<uint32_t>(tx->getFullFee()) * feeMult + extraFee);
    getSignatures(tx).clear();
    tx->addSignature(src);
    return tx;
}

static TransactionTestFramePtr
makeSelfPayment(stellar::TestAccount& account, int nbOps, uint32_t fee)
{
    std::vector<stellar::Operation> ops;
    for (int i = 0; i < nbOps; i++)
    {
        ops.emplace_back(payment(account, i + 1000));
    }
    auto tx = account.tx(ops);
    setFullFee(tx, fee);
    getSignatures(tx).clear();
    tx->addSignature(account);
    return tx;
}

static void
testTxSet(uint32 protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 15;
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = protocolVersion;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    // set up world
    auto root = TestAccount::createRoot(*app);

    const int nbAccounts = 3;

    std::vector<TestAccount> accounts;

    const int64_t minBalance0 = app->getLedgerManager().getLastMinBalance(0);

    int64_t accountBalance =
        app->getLedgerManager().getLastTxFee() + minBalance0;

    std::vector<TransactionFrameBasePtr> txs;
    auto genTx = [&]() {
        std::string accountName = fmt::format("A{}", accounts.size());
        accounts.push_back(root.create(accountName.c_str(), accountBalance));
        auto& account = accounts.back();

        // payment to self
        txs.push_back(account.tx({payment(account.getPublicKey(), 10000)}));
    };
    for (size_t i = 0; i < nbAccounts; i++)
    {
        genTx();
    }
    SECTION("valid set")
    {
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;
        REQUIRE(txSet->sizeTxTotal() == nbAccounts);
    }

    SECTION("too many txs")
    {
        while (txs.size() <= cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE * 2)
        {
            genTx();
        }
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;
        REQUIRE(txSet->sizeTxTotal() == cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE);
    }
    SECTION("invalid tx")
    {
        SECTION("no user")
        {
            auto newUser = TestAccount{*app, getAccount("doesnotexist")};
            txs.push_back(newUser.tx({payment(root, 1)}));
            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
            REQUIRE(removed.size() == 1);
            REQUIRE(txSet->sizeTxTotal() == nbAccounts);
        }
        SECTION("sequence gap")
        {
            auto txPtr = std::const_pointer_cast<TransactionFrameBase>(txs[0]);
            setSeqNum(std::static_pointer_cast<TransactionTestFrame>(txPtr),
                      txs[0]->getSeqNum() + 5);

            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
            REQUIRE(removed.size() == 1);
            REQUIRE(txSet->sizeTxTotal() == nbAccounts - 1);
        }
        SECTION("insufficient balance")
        {
            accounts.push_back(root.create("insufficient", accountBalance - 1));
            txs.back() = accounts.back().tx(
                {payment(accounts.back().getPublicKey(), 10000)});

            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
            REQUIRE(removed.size() == 1);
            REQUIRE(txSet->sizeTxTotal() == nbAccounts - 1);
        }
        SECTION("bad signature")
        {
            auto tx =
                std::static_pointer_cast<TransactionTestFrame const>(txs[0]);
            setMaxTime(tx, UINT64_MAX);
            tx->clearCached();
            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions(txs, *app, 0, 0, removed).second;
            REQUIRE(removed.size() == 1);
            REQUIRE(txSet->sizeTxTotal() == nbAccounts - 1);
        }
    }
}

static TransactionTestFramePtr
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

    auto compareTxs = [](TxFrameList const& actual,
                         TxFrameList const& expected) {
        auto actualNormalized = actual;
        auto expectedNormalized = expected;
        std::sort(actualNormalized.begin(), actualNormalized.end());
        std::sort(expectedNormalized.begin(), expectedNormalized.end());
        REQUIRE(actualNormalized == expectedNormalized);
    };

    SECTION("invalid transaction")
    {
        SECTION("one fee bump")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, minBalance2);
            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({fb1}, *app, 0, 0, invalidTxs);
            compareTxs(invalidTxs, {fb1});
        }

        SECTION("two fee bumps with same sources, first has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, minBalance2);
            auto tx2 = transaction(*app, account1, 2, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, 200);
            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
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
            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
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
            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
            compareTxs(invalidTxs, {fb2});
        }

        SECTION("two fee bumps with same fee source but different source, "
                "second has high fee")
        {
            auto tx1 = transaction(*app, account1, 1, 1, 100);
            auto fb1 = feeBump(*app, account2, tx1, 200);
            auto tx2 = transaction(*app, account2, 1, 1, 100);
            auto fb2 = feeBump(*app, account2, tx2, minBalance2);
            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
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
            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({fb1, fb2}, *app, 0, 0, invalidTxs);
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
            TxFrameList invalidTxs;
            auto txSet = makeTxSetFromTransactions({fb1, fb2, fb3}, *app, 0, 0,
                                                   invalidTxs);
            compareTxs(invalidTxs, {fb2, fb3});
        }
    }
}

TEST_CASE("txset", "[herder][txset]")
{
    SECTION("generalized tx set protocol")
    {
        testTxSet(static_cast<uint32>(SOROBAN_PROTOCOL_VERSION));
    }
    SECTION("protocol current")
    {
        testTxSet(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
        testTxSetWithFeeBumps(Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    }
}

TEST_CASE("txset with PreconditionsV2", "[herder][txset]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
    auto root = TestAccount::createRoot(*app);
    auto a1 = root.create("a1", minBalance2);
    auto a2 = root.create("a2", minBalance2);

    // Move close time past 0
    closeLedgerOn(*app, 1, 1, 2022);

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
                    *app, app->getLedgerManager().getLastClosedLedgerNum() + 1,
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
            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions({txInvalid}, *app, 0, 0, removed)
                    .second;
            REQUIRE(removed.back() == txInvalid);
            REQUIRE(txSet->sizeTxTotal() == 0);

            auto tx1 = transactionWithV2Precondition(*app, a1, 1, 100,
                                                     minSeqAgeCond(minGap));

            // only the first tx can have minSeqAge set
            auto tx2Invalid = transactionWithV2Precondition(
                *app, a2, 2, 100, minSeqAgeCond(minGap));

            auto fb1 = feeBump(*app, a1, tx1, 200);
            auto fb2Invalid = feeBump(*app, a2, tx2Invalid, 200);

            removed.clear();
            if (minSeqNumTxIsFeeBump)
            {
                txSet = makeTxSetFromTransactions({fb1, fb2Invalid}, *app, 0, 0,
                                                  removed)
                            .second;
            }
            else
            {
                txSet = makeTxSetFromTransactions({tx1, tx2Invalid}, *app, 0, 0,
                                                  removed)
                            .second;
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
                *app, a2, 1, 100, ledgerBoundsCond(lclNum + 2, 0));
            TxFrameList removed;
            auto txSet = makeTxSetFromTransactions({tx1, txInvalid}, *app, 0, 0,
                                                   removed);
            REQUIRE(removed.back() == txInvalid);

            // the highest minLedger can be is lcl + 1 because
            // validation is done against the next ledger
            auto tx2 = transactionWithV2Precondition(
                *app, a2, 1, 100, ledgerBoundsCond(lclNum + 1, 0));
            removed.clear();
            txSet = makeTxSetFromTransactions({tx1, tx2}, *app, 0, 0, removed);
            REQUIRE(removed.empty());
        }
        SECTION("maxLedger")
        {
            auto txInvalid = transactionWithV2Precondition(
                *app, a2, 1, 100, ledgerBoundsCond(0, lclNum));
            TxFrameList removed;
            auto txSet = makeTxSetFromTransactions({tx1, txInvalid}, *app, 0, 0,
                                                   removed);
            REQUIRE(removed.back() == txInvalid);

            // the lower maxLedger can be is lcl + 2, as the current
            // ledger is lcl + 1 and maxLedger bound is exclusive.
            auto tx2 = transactionWithV2Precondition(
                *app, a2, 1, 100, ledgerBoundsCond(0, lclNum + 2));
            removed.clear();
            txSet = makeTxSetFromTransactions({tx1, tx2}, *app, 0, 0, removed);
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
                TxFrameList removed;
                auto txSet =
                    makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
                REQUIRE(removed.empty());
            }
            SECTION("fail")
            {
                TxFrameList removed;
                auto txSet =
                    makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
                REQUIRE(removed.back() == tx);
            }
        }
        SECTION("two extra signers")
        {
            SignerKey a2Signer;
            a2Signer.type(SIGNER_KEY_TYPE_ED25519);
            a2Signer.ed25519() = a2.getPublicKey().ed25519();

            cond.extraSigners.emplace_back(a2Signer);
            auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);
            tx->addSignature(root.getSecretKey());

            SECTION("success")
            {
                tx->addSignature(a2.getSecretKey());
                TxFrameList removed;
                auto txSet =
                    makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
                REQUIRE(removed.empty());
            }
            SECTION("fail")
            {
                TxFrameList removed;
                auto txSet =
                    makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
                REQUIRE(removed.back() == tx);
            }
        }
        SECTION("duplicate extra signers")
        {
            cond.extraSigners.emplace_back(rootSigner);
            auto txDupeSigner =
                transactionWithV2Precondition(*app, a1, 1, 100, cond);
            txDupeSigner->addSignature(root.getSecretKey());
            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions({txDupeSigner}, *app, 0, 0, removed);
            REQUIRE(removed.back() == txDupeSigner);
            REQUIRE(txDupeSigner->getResultCode() == txMALFORMED);
        }
        SECTION("signer overlap with default account signer")
        {
            auto rootTx =
                transactionWithV2Precondition(*app, root, 1, 100, cond);
            TxFrameList removed;
            auto txSet =
                makeTxSetFromTransactions({rootTx}, *app, 0, 0, removed);
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

                TxFrameList removed;
                auto txSet =
                    makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
                REQUIRE(removed.empty());
            }
            SECTION("signature missing")
            {
                TxFrameList removed;
                auto txSet =
                    makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
                REQUIRE(removed.back() == tx);
            }
        }
        SECTION("signer overlap with added account signer - both "
                "signers used")
        {
            auto sk1 = makeSigner(root, 100);
            a1.setOptions(setSigner(sk1));

            auto tx = transactionFrameFromOps(app->getNetworkID(), a1,
                                              {root.op(payment(a1, 1))}, {root},
                                              cond);

            TxFrameList removed;
            auto txSet = makeTxSetFromTransactions({tx}, *app, 0, 0, removed);
            REQUIRE(removed.empty());
        }
    }
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
        auto [txSet, applicableTxSet] =
            makeTxSetFromTransactions(txs, *app, 0, 0);
        REQUIRE(applicableTxSet->size(lhCopy) == lim);
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
    //    should add 52 tx (104 ops)

    //  surge threshold is 112-100=12 ops
    //     surge pricing @ 12 (2 extra tx)

    uint32 const baseCount = 8;
    uint32 const extraTx = 52;
    uint32 const newCount = 56; // 112/2
    SECTION("surged")
    {
        SECTION("mixed")
        {
            SECTION("generalized tx set protocol")
            {
                SECTION("fitting exactly into capacity does not cause surge")
                {
                    testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                                baseCount, extraTx, maxTxSetSize, 100, 200);
                }
                SECTION("evicting one tx causes surge")
                {
                    testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                                baseCount + 1, extraTx, maxTxSetSize, 1000,
                                2000, 1);
                }
            }
            SECTION("protocol current")
            {
                if (protocolVersionStartsFrom(
                        Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                        SOROBAN_PROTOCOL_VERSION))
                {
                    SECTION(
                        "fitting exactly into capacity does not cause surge")
                    {
                        testBaseFee(
                            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                            baseCount, extraTx, maxTxSetSize, 100, 200);
                    }
                    SECTION("evicting one tx causes surge")
                    {
                        testBaseFee(
                            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                            baseCount + 1, extraTx, maxTxSetSize, 1000, 2000,
                            1);
                    }
                }
                else
                {
                    SECTION("maxed out surged")
                    {
                        testBaseFee(
                            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1,
                            baseCount, extraTx, maxTxSetSize, 1000, 2000);
                    }
                    SECTION("smallest surged")
                    {
                        testBaseFee(
                            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1,
                            baseCount + 1, extraTx - 50, maxTxSetSize - 100 + 1,
                            1000, 2000);
                    }
                }
            }
        }
        SECTION("newOnly")
        {
            SECTION("generalized tx set protocol")
            {
                SECTION("fitting exactly into capacity does not cause surge")
                {
                    testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                                0, newCount, maxTxSetSize, 200, 200);
                }
                SECTION("evicting one tx causes surge")
                {
                    testBaseFee(static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION),
                                0, newCount + 1, maxTxSetSize, 20002, 20002, 1);
                }
            }
            SECTION("protocol current")
            {
                if (protocolVersionStartsFrom(
                        Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                        SOROBAN_PROTOCOL_VERSION))
                {
                    SECTION(
                        "fitting exactly into capacity does not cause surge")
                    {
                        testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                                    newCount, maxTxSetSize, 200, 200);
                    }
                    SECTION("evicting one tx causes surge")
                    {
                        testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                                    newCount + 1, maxTxSetSize, 20002, 20002,
                                    1);
                    }
                }
                else
                {
                    testBaseFee(
                        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1, 0,
                        newCount, maxTxSetSize, 20001, 20002);
                }
            }
        }
    }
    SECTION("not surged")
    {
        SECTION("mixed")
        {
            SECTION("protocol current")
            {
                // baseFee = minFee = 100
                // high = 2*minFee
                // highest number of ops not surged is max-100
                testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, baseCount,
                            extraTx - 50, maxTxSetSize - 100, 100, 200);
            }
        }
        SECTION("newOnly")
        {
            SECTION("protocol current")
            {
                // low = minFee = 100
                // high = 2*minFee
                // highest number of ops not surged is max-100
                testBaseFee(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 0,
                            newCount - 50, maxTxSetSize - 100, 200, 200);
            }
        }
    }
}

TEST_CASE("tx set hits overlay byte limit during construction",
          "[transactionqueue][soroban]")
{
    Config cfg(getTestConfig());
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
    auto max = std::numeric_limits<uint32_t>::max();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = max;

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);

    modifySorobanNetworkConfig(*app, [max](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxTxCount = max;
        cfg.mLedgerMaxReadLedgerEntries = max;
        cfg.mLedgerMaxReadBytes = max;
        cfg.mLedgerMaxWriteLedgerEntries = max;
        cfg.mLedgerMaxWriteBytes = max;
        cfg.mLedgerMaxTransactionsSizeBytes = max;
        cfg.mLedgerMaxInstructions = max;
    });

    auto const& conf =
        app->getLedgerManager().getSorobanNetworkConfigReadOnly();
    uint32_t maxContractSize = 0;
    maxContractSize = conf.maxContractSizeBytes();

    auto makeTx = [&](TestAccount& acc, TxSetPhase const& phase) {
        if (phase == TxSetPhase::SOROBAN)
        {
            SorobanResources res;
            res.instructions = 1;
            res.readBytes = 0;
            res.writeBytes = 0;

            return createUploadWasmTx(*app, acc, 100,
                                      DEFAULT_TEST_RESOURCE_FEE * 10, res,
                                      std::nullopt, 0, maxContractSize);
        }
        else
        {
            return makeMultiPayment(acc, acc, 100, 1, 100, 1);
        }
    };

    auto testPhaseWithOverlayLimit = [&](TxSetPhase const& phase) {
        TxFrameList txs;
        size_t totalSize = 0;
        int txCount = 0;

        while (totalSize < MAX_TX_SET_ALLOWANCE)
        {
            auto a = root.create(fmt::format("A{}", txCount++), 500000000);
            txs.emplace_back(makeTx(a, phase));
            totalSize += xdr::xdr_size(txs.back()->getEnvelope());
        }

        PerPhaseTransactionList invalidPhases;
        invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));

        PerPhaseTransactionList phases;
        if (phase == TxSetPhase::SOROBAN)
        {
            phases = PerPhaseTransactionList{{}, txs};
        }
        else
        {
            phases = PerPhaseTransactionList{txs, {}};
        }

        auto [txSet, applicableTxSet] =
            makeTxSetFromTransactions(phases, *app, 0, 0, invalidPhases);
        REQUIRE(txSet->encodedSize() <= MAX_MESSAGE_SIZE);

        REQUIRE(invalidPhases[static_cast<size_t>(phase)].empty());
        auto const& phaseTxs = applicableTxSet->getPhase(phase);
        auto trimmedSize =
            std::accumulate(phaseTxs.begin(), phaseTxs.end(), size_t(0),
                            [&](size_t a, TransactionFrameBasePtr const& tx) {
                                return a += xdr::xdr_size(tx->getEnvelope());
                            });

        auto byteAllowance = phase == TxSetPhase::SOROBAN
                                 ? MAX_SOROBAN_BYTE_ALLOWANCE
                                 : MAX_CLASSIC_BYTE_ALLOWANCE;
        REQUIRE(trimmedSize > byteAllowance - conf.txMaxSizeBytes());
        REQUIRE(trimmedSize <= byteAllowance);
    };

    SECTION("soroban")
    {
        testPhaseWithOverlayLimit(TxSetPhase::SOROBAN);
    }
    SECTION("classic")
    {
        testPhaseWithOverlayLimit(TxSetPhase::CLASSIC);
    }
}

TEST_CASE("surge pricing", "[herder][txset][soroban]")
{
    SECTION("max 0 ops per ledger")
    {
        Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY));
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 0;

        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        auto root = TestAccount::createRoot(*app);

        auto destAccount = root.create("destAccount", 500000000);

        SECTION("classic")
        {
            auto tx = makeMultiPayment(destAccount, root, 1, 100, 0, 1);

            TxFrameList invalidTxs;
            auto txSet =
                makeTxSetFromTransactions({tx}, *app, 0, 0, invalidTxs).second;

            // Transaction is valid, but trimmed by surge pricing.
            REQUIRE(invalidTxs.empty());
            REQUIRE(txSet->sizeTxTotal() == 0);
        }
        SECTION("soroban")
        {
            uint32_t const baseFee = 10'000'000;
            modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& cfg) {
                cfg.mLedgerMaxTxCount = 0;
            });
            SorobanResources resources;
            auto sorobanTx = createUploadWasmTx(
                *app, root, baseFee, DEFAULT_TEST_RESOURCE_FEE, resources);

            PerPhaseTransactionList invalidTxs;
            invalidTxs.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
            auto txSet = makeTxSetFromTransactions(
                             PerPhaseTransactionList{{}, {sorobanTx}}, *app, 0,
                             0, invalidTxs)
                             .second;

            // Transaction is valid, but trimmed by surge pricing.
            REQUIRE(std::all_of(invalidTxs.begin(), invalidTxs.end(),
                                [](auto const& txs) { return txs.empty(); }));
            REQUIRE(txSet->sizeTxTotal() == 0);
        }
    }
    SECTION("soroban txs")
    {
        Config cfg(getTestConfig());
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION);
        // Max 1 classic op
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1;

        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);
        // Max 2 soroban ops
        modifySorobanNetworkConfig(
            *app, [](SorobanNetworkConfig& cfg) { cfg.mLedgerMaxTxCount = 2; });

        auto root = TestAccount::createRoot(*app);
        auto acc1 = root.create("account1", 500000000);
        auto acc2 = root.create("account2", 500000000);
        auto acc3 = root.create("account3", 500000000);
        auto acc4 = root.create("account4", 500000000);
        auto acc5 = root.create("account5", 500000000);
        auto acc6 = root.create("account6", 500000000);

        // Ensure these accounts don't overlap with classic tx (with root source
        // account)
        std::vector<TestAccount> accounts = {acc1, acc2, acc3,
                                             acc4, acc5, acc6};

        // Valid classic
        auto tx = makeMultiPayment(acc1, root, 1, 100, 0, 1);

        SorobanNetworkConfig conf =
            app->getLedgerManager().getSorobanNetworkConfigReadOnly();

        uint32_t const baseFee = 10'000'000;
        SorobanResources resources;
        resources.instructions = 800'000;
        resources.readBytes = conf.txMaxReadBytes();
        resources.writeBytes = 1000;
        auto sorobanTx = createUploadWasmTx(
            *app, acc2, baseFee, DEFAULT_TEST_RESOURCE_FEE, resources);

        auto generateTxs = [&](std::vector<TestAccount>& accounts,
                               SorobanNetworkConfig conf) {
            TxFrameList txs;
            for (auto& acc : accounts)
            {
                SorobanResources res;
                res.instructions = rand_uniform<uint32_t>(
                    1, static_cast<uint32>(conf.txMaxInstructions()));
                res.readBytes =
                    rand_uniform<uint32_t>(1, conf.txMaxReadBytes());
                res.writeBytes =
                    rand_uniform<uint32_t>(1, conf.txMaxWriteBytes());
                auto read =
                    rand_uniform<uint32_t>(0, conf.txMaxReadLedgerEntries());
                auto write = rand_uniform<uint32_t>(
                    0, std::min(conf.txMaxWriteLedgerEntries(),
                                (conf.txMaxReadLedgerEntries() - read)));
                for (auto const& key :
                     LedgerTestUtils::generateUniqueValidSorobanLedgerEntryKeys(
                         write))
                {
                    res.footprint.readWrite.emplace_back(key);
                }
                for (auto const& key :
                     LedgerTestUtils::generateUniqueValidSorobanLedgerEntryKeys(
                         read))
                {
                    res.footprint.readOnly.emplace_back(key);
                }

                auto tx = createUploadWasmTx(*app, acc, baseFee * 10,
                                             /* refundableFee */ baseFee, res);
                if (rand_flip())
                {
                    txs.emplace_back(tx);
                }
                else
                {
                    // Double the inclusion fee
                    txs.emplace_back(feeBump(*app, acc, tx, baseFee * 10 * 2));
                }
                CLOG_INFO(Herder,
                          "Generated tx with {} instructions, {} read "
                          "bytes, {} write bytes, data bytes, {} read "
                          "ledger entries, {} write ledger entries",
                          res.instructions, res.readBytes, res.writeBytes, read,
                          write);
            }
            return txs;
        };

        SECTION("invalid soroban is rejected")
        {
            TransactionTestFramePtr invalidSoroban;
            SECTION("invalid fee")
            {
                // Fee too small
                invalidSoroban = createUploadWasmTx(
                    *app, acc2, 100, DEFAULT_TEST_RESOURCE_FEE, resources);
            }
            SECTION("invalid resource")
            {
                // Too many instructions
                resources.instructions = UINT32_MAX;
                invalidSoroban = createUploadWasmTx(
                    *app, acc2, baseFee, DEFAULT_TEST_RESOURCE_FEE, resources);
            }
            PerPhaseTransactionList invalidPhases;
            invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
            auto txSet = makeTxSetFromTransactions(
                             PerPhaseTransactionList{{tx}, {invalidSoroban}},
                             *app, 0, 0, invalidPhases)
                             .second;

            // Soroban tx is rejected
            REQUIRE(txSet->sizeTxTotal() == 1);
            REQUIRE(invalidPhases[0].empty());
            REQUIRE(invalidPhases[1].size() == 1);
            REQUIRE(invalidPhases[1][0]->getFullHash() ==
                    invalidSoroban->getFullHash());
        }
        SECTION("classic and soroban fit")
        {
            PerPhaseTransactionList invalidPhases;
            invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
            auto txSet = makeTxSetFromTransactions(
                             PerPhaseTransactionList{{tx}, {sorobanTx}}, *app,
                             0, 0, invalidPhases)
                             .second;

            // Everything fits
            REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                                [](auto const& txs) { return txs.empty(); }));
            REQUIRE(txSet->sizeTxTotal() == 2);
        }
        SECTION("classic and soroban in the same phase are rejected")
        {
            PerPhaseTransactionList invalidPhases;
            invalidPhases.resize(1);
            REQUIRE_THROWS_AS(makeTxSetFromTransactions(
                                  PerPhaseTransactionList{{tx, sorobanTx}},
                                  *app, 0, 0, invalidPhases),
                              std::runtime_error);
        }
        SECTION("soroban surge pricing, classic unaffected")
        {
            // Another soroban tx with higher fee, which will be selected
            auto sorobanTxHighFee = createUploadWasmTx(
                *app, acc3, baseFee * 2, DEFAULT_TEST_RESOURCE_FEE, resources);
            PerPhaseTransactionList invalidPhases;
            invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
            auto txSet = makeTxSetFromTransactions(
                             PerPhaseTransactionList{
                                 {tx}, {sorobanTx, sorobanTxHighFee}},
                             *app, 0, 0, invalidPhases)
                             .second;

            REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                                [](auto const& txs) { return txs.empty(); }));
            REQUIRE(txSet->sizeTxTotal() == 2);
            auto const& classicTxs =
                txSet->getPhase(TxSetPhase::CLASSIC).getSequentialTxs();
            REQUIRE(classicTxs.size() == 1);
            REQUIRE(classicTxs[0]->getFullHash() == tx->getFullHash());
            auto const& sorobanTxs =
                txSet->getPhase(TxSetPhase::SOROBAN).getSequentialTxs();
            REQUIRE(sorobanTxs.size() == 1);
            REQUIRE(sorobanTxs[0]->getFullHash() ==
                    sorobanTxHighFee->getFullHash());
        }
        SECTION("soroban surge pricing with gap")
        {
            // Another soroban tx with high fee and a bit less resources
            // Still half capacity available
            resources.readBytes = conf.txMaxReadBytes() / 2;
            auto sorobanTxHighFee = createUploadWasmTx(
                *app, acc3, baseFee * 2, DEFAULT_TEST_RESOURCE_FEE, resources);

            // Create another small soroban tx, with small fee. It should be
            // picked up anyway since we can't fit sorobanTx (gaps are allowed)
            resources.instructions = 1;
            resources.readBytes = 1;
            resources.writeBytes = 1;

            auto smallSorobanLowFee = createUploadWasmTx(
                *app, acc4, baseFee / 10, DEFAULT_TEST_RESOURCE_FEE, resources);

            PerPhaseTransactionList invalidPhases;
            invalidPhases.resize(static_cast<size_t>(TxSetPhase::PHASE_COUNT));
            auto txSet =
                makeTxSetFromTransactions(
                    PerPhaseTransactionList{
                        {tx},
                        {sorobanTxHighFee, smallSorobanLowFee, sorobanTx}},
                    *app, 0, 0, invalidPhases)
                    .second;

            REQUIRE(std::all_of(invalidPhases.begin(), invalidPhases.end(),
                                [](auto const& txs) { return txs.empty(); }));
            REQUIRE(txSet->sizeTxTotal() == 3);
            auto const& classicTxs =
                txSet->getPhase(TxSetPhase::CLASSIC).getSequentialTxs();
            REQUIRE(classicTxs.size() == 1);
            REQUIRE(classicTxs[0]->getFullHash() == tx->getFullHash());
            for (auto const& t : txSet->getPhase(TxSetPhase::SOROBAN))
            {
                // smallSorobanLowFee was picked over sorobanTx to fill the gap
                bool pickedGap =
                    t->getFullHash() == sorobanTxHighFee->getFullHash() ||
                    t->getFullHash() == smallSorobanLowFee->getFullHash();
                REQUIRE(pickedGap);
            }
        }
        SECTION("tx set construction limits")
        {
            int const ITERATIONS = 20;
            for (int i = 0; i < ITERATIONS; i++)
            {
                SECTION("iteration " + std::to_string(i))
                {
                    PerPhaseTransactionList invalidPhases;
                    invalidPhases.resize(
                        static_cast<size_t>(TxSetPhase::PHASE_COUNT));
                    auto txSet = makeTxSetFromTransactions(
                                     PerPhaseTransactionList{
                                         {tx}, generateTxs(accounts, conf)},
                                     *app, 0, 0, invalidPhases)
                                     .second;

                    REQUIRE(std::all_of(
                        invalidPhases.begin(), invalidPhases.end(),
                        [](auto const& txs) { return txs.empty(); }));
                    auto const& classicTxs =
                        txSet->getPhase(TxSetPhase::CLASSIC).getSequentialTxs();
                    auto const& sorobanTxs =
                        txSet->getPhase(TxSetPhase::SOROBAN).getSequentialTxs();
                    REQUIRE(classicTxs.size() == 1);
                    REQUIRE(classicTxs[0]->getFullHash() == tx->getFullHash());
                    // Depending on resources generated for each tx, can only
                    // fit 1 or 2 transactions
                    bool expectedSorobanTxs =
                        sorobanTxs.size() == 1 || sorobanTxs.size() == 2;
                    REQUIRE(expectedSorobanTxs);
                }
            }
        }
        SECTION("tx sets over limits are invalid")
        {
            TxFrameList txs = generateTxs(accounts, conf);
            auto txSet =
                testtxset::makeNonValidatedGeneralizedTxSet(
                    {{}, {std::make_pair(500, txs)}}, *app,
                    app->getLedgerManager().getLastClosedLedgerHeader().hash)
                    .second;

            REQUIRE(!txSet->checkValid(*app, 0, 0));
        }
    }
}

TEST_CASE("surge pricing with DEX separation", "[herder][txset]")
{
    if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                SOROBAN_PROTOCOL_VERSION))
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

    auto root = TestAccount::createRoot(*app);

    auto accountA = root.create("accountA", 5000000000);
    auto accountB = root.create("accountB", 5000000000);
    auto accountC = root.create("accountC", 5000000000);
    auto accountD = root.create("accountD", 5000000000);

    auto seqNumA = accountA.getLastSequenceNumber();
    auto seqNumB = accountB.getLastSequenceNumber();
    auto seqNumC = accountC.getLastSequenceNumber();
    auto seqNumD = accountD.getLastSequenceNumber();

    auto runTest = [&](std::vector<TransactionFrameBasePtr> const& txs,
                       size_t expectedTxsA, size_t expectedTxsB,
                       size_t expectedTxsC, size_t expectedTxsD,
                       int64_t expectedNonDexBaseFee,
                       int64_t expectedDexBaseFee) {
        auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;
        size_t cntA = 0, cntB = 0, cntC = 0, cntD = 0;
        auto const& phases = txSet->getPhasesInApplyOrder();

        for (auto const& tx : phases[static_cast<size_t>(TxSetPhase::CLASSIC)])
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
            if (tx->getSourceID() == accountD.getPublicKey())
            {
                ++cntD;
                ++seqNumD;
                REQUIRE(seqNumD == tx->getSeqNum());
            }

            auto baseFee = txSet->getTxBaseFee(tx);
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
        REQUIRE(cntD == expectedTxsD);
    };

    auto nonDexTx = [](TestAccount& account, uint32 nbOps, uint32_t opFee) {
        return makeSelfPayment(account, nbOps, opFee * nbOps);
    };
    auto dexTx = [&](TestAccount& account, uint32 nbOps, uint32_t opFee) {
        return createSimpleDexTx(*app, account, nbOps, opFee * nbOps);
    };
    SECTION("only non-DEX txs")
    {
        runTest({nonDexTx(accountA, 8, 200), nonDexTx(accountB, 4, 300),
                 nonDexTx(accountC, 2, 400),
                 /* cutoff */
                 nonDexTx(accountD, 2, 100)},
                1, 1, 1, 0, 200, 0);
    }
    SECTION("only DEX txs")
    {
        runTest({dexTx(accountA, 2, 200), dexTx(accountB, 1, 300),
                 dexTx(accountC, 2, 400),
                 /* cutoff */
                 dexTx(accountD, 1, 100)},
                1, 1, 1, 0, 0, 200);
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
                        nonDexTx(accountA, 6, 100),
                        dexTx(accountB, 5, 400),
                        /* cutoff */
                        dexTx(accountC, 1, 200),
                        dexTx(accountD, 1, 399),
                    },
                    1, 1, 0, 0, 100, 400);
            }
            SECTION("both limits reached, but only DEX evicted")
            {
                runTest(
                    {
                        /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                        nonDexTx(accountA, 10, 100),
                        dexTx(accountB, 5, 400),
                        /* cutoff */
                        dexTx(accountC, 1, 399),
                        dexTx(accountD, 1, 399),
                    },
                    1, 1, 0, 0, 100, 400);
            }
        }
        SECTION("all txs surge priced")
        {
            SECTION("only global limit reached")
            {
                runTest(
                    {
                        /* 13 non-DEX ops + 2 DEX ops = 15 ops */
                        nonDexTx(accountA, 13, 250),
                        dexTx(accountB, 2, 250),
                        /* cutoff */
                        dexTx(accountC, 1, 200),
                        nonDexTx(accountD, 1, 249),
                    },
                    1, 1, 0, 0, 250, 250);
            }
            SECTION("both limits reached")
            {
                SECTION("non-DEX fee is lowest")
                {
                    runTest(
                        {
                            /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                            nonDexTx(accountA, 10, 250),
                            dexTx(accountB, 5, 400),
                            /* cutoff */
                            dexTx(accountC, 1, 399),
                            nonDexTx(accountD, 1, 249),
                        },
                        1, 1, 0, 0, 250, 400);
                }
                SECTION("DEX fee is lowest")
                {
                    runTest(
                        {
                            /* 10 non-DEX ops + 5 DEX ops = 15 ops */
                            nonDexTx(accountA, 10, 500),
                            dexTx(accountB, 5, 200),
                            /* cutoff */
                            dexTx(accountC, 1, 199),
                            nonDexTx(accountD, 1, 199),
                        },
                        1, 1, 0, 0, 200, 200);
                }
            }
        }
    }
}

TEST_CASE("surge pricing with DEX separation holds invariants",
          "[herder][txset]")
{
    if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                SOROBAN_PROTOCOL_VERSION))
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
            uint32 ops = numOpsDistr(Catch::rng());
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
            auto txSet = makeTxSetFromTransactions(txs, *app, 0, 0).second;

            auto const& phases = txSet->getPhasesInApplyOrder();
            std::array<uint32_t, 2> opsCounts{};
            std::array<int64_t, 2> baseFees{};

            for (auto const& resTx :
                 phases[static_cast<size_t>(TxSetPhase::CLASSIC)])
            {
                auto isDex = static_cast<size_t>(resTx->hasDexOperations());
                opsCounts[isDex] += resTx->getNumOperations();
                auto baseFee = txSet->getTxBaseFee(resTx);
                REQUIRE(baseFee);
                if (baseFees[isDex] != 0)
                {
                    // All base fees should be the same among the
                    // transaction categories.
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

TEST_CASE("generalized tx set applied to ledger", "[herder][txset][soroban]")
{
    Config cfg(getTestConfig());
    cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = true;
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    overrideSorobanNetworkConfigForTest(*app);
    int64 startingBalance =
        app->getLedgerManager().getLastMinBalance(0) + 10000000;

    std::vector<TestAccount> accounts;
    int txCnt = 0;
    auto addTx = [&](int nbOps, uint32_t fee) {
        auto account = root.create(std::to_string(txCnt++), startingBalance);
        accounts.push_back(account);
        return makeSelfPayment(account, nbOps, fee);
    };

    SorobanResources resources;
    resources.instructions = 3'000'000;
    resources.readBytes = 0;
    resources.writeBytes = 2000;
    auto dummyAccount = root.create("dummy", startingBalance);
    auto dummyUploadTx =
        createUploadWasmTx(*app, dummyAccount, 100, 1000, resources);
    resources.footprint.readWrite.emplace_back();
    auto resourceFee = sorobanResourceFee(
        *app, resources, xdr::xdr_size(dummyUploadTx->getEnvelope()), 40);
    // This value should not be changed for the test setup, but if it ever
    // is changed,/ then we'd need to compute the rent fee via the rust bridge
    // function (which is a bit verbose).
    uint32_t const rentFee = 20'048;
    resourceFee += rentFee;
    resources.footprint.readWrite.pop_back();
    auto addSorobanTx = [&](uint32_t inclusionFee) {
        auto account = root.create(std::to_string(txCnt++), startingBalance);
        accounts.push_back(account);
        return createUploadWasmTx(*app, account, inclusionFee, resourceFee,
                                  resources);
    };

    auto checkFees = [&](std::pair<TxSetXDRFrameConstPtr,
                                   ApplicableTxSetFrameConstPtr> const& txSet,
                         std::vector<int64_t> const& expectedFeeCharged,
                         bool validateTxSet = true) {
        if (validateTxSet)
        {
            REQUIRE(txSet.second->checkValid(*app, 0, 0));
        }

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
                      getTestDate(13, 4, 2022), txSet.first);

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
            {{std::make_pair(
                 1000, std::vector<TransactionFrameBasePtr>{addTx(3, 3500),
                                                            addTx(2, 5000)})},
             {}},
            *app, app->getLedgerManager().getLastClosedLedgerHeader().hash);
        checkFees(txSet, {3000, 2000});
    }
    SECTION("single non-discounted component")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {{std::make_pair(std::nullopt,
                             std::vector<TransactionFrameBasePtr>{
                                 addTx(3, 3500), addTx(2, 5000)})},
             {}},
            *app, app->getLedgerManager().getLastClosedLedgerHeader().hash);
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
            {components, {}}, *app,
            app->getLedgerManager().getLastClosedLedgerHeader().hash);
        checkFees(txSet, {3000, 2000, 500, 2500, 8000, 35000, 10000});
    }
    SECTION("soroban")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {
                {std::make_pair(1000,
                                std::vector<TransactionFrameBasePtr>{
                                    addTx(3, 3500), addTx(2, 5000)})},
                {std::make_pair(2000,
                                std::vector<TransactionFrameBasePtr>{
                                    addSorobanTx(5000), addSorobanTx(10000)})},
            },
            *app, app->getLedgerManager().getLastClosedLedgerHeader().hash);
        SECTION("with validation")
        {
            checkFees(txSet,
                      {3000, 2000, 2000 + resourceFee, 2000 + resourceFee});
        }
        SECTION("without validation")
        {
            checkFees(txSet,
                      {3000, 2000, 2000 + resourceFee, 2000 + resourceFee},
                      /* validateTxSet */ false);
        }
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
    std::vector<TestAccount> accounts;
    for (int i = 0; i < 1000; ++i)
    {
        std::string accountName = fmt::format("A{}", accounts.size());
        accounts.push_back(root.create(accountName.c_str(), 500000000));
    }

    using TxPair = std::pair<Value, TxSetXDRFrameConstPtr>;
    auto makeTxUpgradePair = [&](HerderImpl& herder,
                                 TxSetXDRFrameConstPtr txSet,
                                 uint64_t closeTime,
                                 SVUpgrades const& upgrades) {
        StellarValue sv = herder.makeStellarValue(
            txSet->getContentsHash(), closeTime, upgrades, root.getSecretKey());
        auto v = xdr::xdr_to_opaque(sv);
        return TxPair{v, txSet};
    };
    auto makeTxPair = [&](HerderImpl& herder, TxSetXDRFrameConstPtr txSet,
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
        std::vector<TransactionFrameBasePtr> txs(n);
        size_t index = 0;

        std::generate(std::begin(txs), std::end(txs), [&]() {
            accounts[index].loadSequenceNumber();
            return makeMultiPayment(root, accounts[index++], nbOps, 1000, 0,
                                    feeMulti);
        });

        return makeTxSetFromTransactions(txs, *app, 0, 0);
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
            auto [txSet, applicableTxSet] =
                makeTransactions(spec.n, spec.nbOps, spec.feeMulti);
            txSetHashes.push_back(txSet->getContentsHash());
            txSetSizes.push_back(applicableTxSet->size(lcl.header));
            txSetOpSizes.push_back(applicableTxSet->sizeOpTotal());
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

        auto txSetL = makeTransactions(maxTxSetSize, 1, 101).first;
        addToCandidates(makeTxPair(herder, txSetL, 20));
        auto txSetL2 = makeTransactions(maxTxSetSize, 1, 1000).first;
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

        auto txSet0 = makeTransactions(0, 1, 100).first;
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
            auto& sig = tx->getMutableEnvelope().type() == ENVELOPE_TYPE_TX_V0
                            ? tx->getMutableEnvelope().v0().signatures
                            : tx->getMutableEnvelope().v1().signatures;
            sig.clear();
            tx->addSignature(root.getSecretKey());
            auto [txSet, applicableTxSet] =
                testtxset::makeNonValidatedTxSetBasedOnLedgerVersion(
                    {tx}, *app,
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
            // makeTxSetFromTransactions() trims the transaction if
            // and only if we expect it to be invalid.
            auto closeTimeOffset = nextCloseTime - lclCloseTime;
            TxFrameList removed;
            TxSetUtils::trimInvalid(
                applicableTxSet->getPhase(TxSetPhase::CLASSIC)
                    .getSequentialTxs(),
                *app, closeTimeOffset, closeTimeOffset, removed);
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
        auto transactions1 = makeTransactions(5, 1, 100).first;
        auto transactions2 = makeTransactions(4, 1, 100).first;

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

        TxSetXDRFrameConstPtr malformedTxSet;
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
            malformedTxSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
        }
        else
        {
            TransactionSet xdrTxSet;
            transactions1->toXDR(xdrTxSet);
            auto& txs = xdrTxSet.txs;
            std::swap(txs[0], txs[1]);
            malformedTxSet = TxSetXDRFrame::makeFromWire(xdrTxSet);
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
    SECTION("previous protocol")
    {
        testSCPDriver(Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1, 1000, 15);
    }
    SECTION("protocol current")
    {
        testSCPDriver(Config::CURRENT_LEDGER_PROTOCOL_VERSION, 1000, 15);
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
        SECTION("bucketlistDB")
        {
            configure(Config::TestDbMode::TESTDB_BUCKET_DB_PERSISTENT);
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
                    .getLastClosedLedgerNum() >= expectedLedger);
        REQUIRE(sim->getNode(nodeIDs[1])
                    ->getLedgerManager()
                    .getLastClosedLedgerNum() >= expectedLedger);

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
            [&]() { return sim->haveAllExternalized(expectedLedger + 2, 6); },
            2 * numLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        // nodes are at least on ledger 7 (some may be on 8)
        for (int i = 0; i <= 2; i++)
        {
            // All nodes are in sync
            REQUIRE(sim->getNode(nodeIDs[i])->getState() ==
                    Application::State::APP_SYNCED_STATE);
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

TEST_CASE("SCP checkpoint", "[catchup][herder]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    auto histCfg = std::make_shared<TmpDirHistoryConfigurator>();

    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(v0NodeID);

    Config cfg1 = getTestConfig(1);
    Config cfg2 = getTestConfig(2);
    Config cfg3 = getTestConfig(3);

    cfg2.FORCE_SCP = false;
    cfg2.MODE_DOES_CATCHUP = true;
    cfg3.FORCE_SCP = false;
    cfg3.MODE_DOES_CATCHUP = true;
    cfg1.MODE_DOES_CATCHUP = false;

    cfg1 = histCfg->configure(cfg1, true);
    cfg3 = histCfg->configure(cfg3, false);
    cfg2 = histCfg->configure(cfg2, false);

    auto mainNode = simulation->addNode(v0SecretKey, qSet, &cfg1);
    simulation->startAllNodes();
    auto firstCheckpoint = HistoryManager::firstLedgerAfterCheckpointContaining(
        1, mainNode->getConfig());

    // Crank until we are halfway through the second checkpoint
    simulation->crankUntil(
        [&]() {
            return simulation->haveAllExternalized(firstCheckpoint + 32, 1);
        },
        2 * (firstCheckpoint + 32) * Herder::EXP_LEDGER_TIMESPAN_SECONDS,
        false);

    SECTION("GC old checkpoints")
    {
        HerderImpl& herder = static_cast<HerderImpl&>(mainNode->getHerder());

        // Should have MAX_SLOTS_TO_REMEMBER slots + checkpoint slot
        REQUIRE(herder.getSCP().getKnownSlotsCount() ==
                mainNode->getConfig().MAX_SLOTS_TO_REMEMBER + 1);

        auto secondCheckpoint =
            HistoryManager::firstLedgerAfterCheckpointContaining(
                firstCheckpoint, mainNode->getConfig());

        // Crank until we complete the 2nd checkpoint
        simulation->crankUntil(
            [&]() {
                return simulation->haveAllExternalized(secondCheckpoint, 1);
            },
            2 * 32 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        REQUIRE(mainNode->getLedgerManager().getLastClosedLedgerNum() ==
                secondCheckpoint);

        // Checkpoint is within [lcl, lcl - MAX_SLOTS_TO_REMEMBER], so we
        // should only have MAX_SLOTS_TO_REMEMBER slots
        REQUIRE(herder.getSCP().getKnownSlotsCount() ==
                mainNode->getConfig().MAX_SLOTS_TO_REMEMBER);
    }

    SECTION("Out of sync node receives checkpoint")
    {
        // Start out of sync node
        auto outOfSync = simulation->addNode(v1SecretKey, qSet, &cfg2);
        simulation->addPendingConnection(v0NodeID, v1NodeID);
        simulation->startAllNodes();
        auto& lam = static_cast<LedgerApplyManagerImpl&>(
            outOfSync->getLedgerApplyManager());

        // Crank until outOfSync node has recieved checkpoint ledger and started
        // catchup
        simulation->crankUntil([&]() { return lam.isCatchupInitialized(); },
                               2 * Herder::SEND_LATEST_CHECKPOINT_DELAY, false);

        auto const& bufferedLedgers = lam.getBufferedLedgers();
        REQUIRE(!bufferedLedgers.empty());
        REQUIRE(bufferedLedgers.begin()->first == firstCheckpoint);
        REQUIRE(bufferedLedgers.crbegin()->first ==
                mainNode->getLedgerManager().getLastClosedLedgerNum());
    }

    SECTION("Two out of sync nodes receive checkpoint")
    {
        // Start two out of sync nodes
        auto outOfSync1 = simulation->addNode(v1SecretKey, qSet, &cfg2);
        auto outOfSync2 = simulation->addNode(v2SecretKey, qSet, &cfg3);

        simulation->addPendingConnection(v0NodeID, v1NodeID);
        simulation->addPendingConnection(v0NodeID, v2NodeID);

        simulation->startAllNodes();
        auto& cm1 = static_cast<LedgerApplyManagerImpl&>(
            outOfSync1->getLedgerApplyManager());
        auto& cm2 = static_cast<LedgerApplyManagerImpl&>(
            outOfSync2->getLedgerApplyManager());

        // Crank until outOfSync node has recieved checkpoint ledger and started
        // catchup
        simulation->crankUntil(
            [&]() {
                return cm1.isCatchupInitialized() && cm2.isCatchupInitialized();
            },
            2 * Herder::SEND_LATEST_CHECKPOINT_DELAY, false);

        auto const& bufferedLedgers1 = cm1.getBufferedLedgers();
        REQUIRE(!bufferedLedgers1.empty());
        REQUIRE(bufferedLedgers1.begin()->first == firstCheckpoint);
        REQUIRE(bufferedLedgers1.crbegin()->first ==
                mainNode->getLedgerManager().getLastClosedLedgerNum());
        auto const& bufferedLedgers2 = cm2.getBufferedLedgers();
        REQUIRE(!bufferedLedgers2.empty());
        REQUIRE(bufferedLedgers2.begin()->first == firstCheckpoint);
        REQUIRE(bufferedLedgers2.crbegin()->first ==
                mainNode->getLedgerManager().getLastClosedLedgerNum());
    }
}

// This test confirms that tx set processing and consensus are independent of
// the tx queue source account limit (for now)
TEST_CASE("tx queue source account limit", "[herder][transactionqueue]")
{
    std::shared_ptr<Simulation> simulation;
    std::shared_ptr<Application> app;

    auto setup = [&]() {
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        simulation = std::make_shared<Simulation>(
            Simulation::OVER_LOOPBACK, networkID, [](int i) {
                auto cfg = getTestConfig(i, Config::TESTDB_DEFAULT);
                return cfg;
            });

        auto validatorAKey = SecretKey::fromSeed(sha256("validator-A"));
        auto validatorBKey = SecretKey::fromSeed(sha256("validator-B"));
        auto validatorCKey = SecretKey::fromSeed(sha256("validator-C"));

        SCPQuorumSet qset;
        // Everyone needs to vote to proceed
        qset.threshold = 3;
        qset.validators.push_back(validatorAKey.getPublicKey());
        qset.validators.push_back(validatorBKey.getPublicKey());
        qset.validators.push_back(validatorCKey.getPublicKey());

        simulation->addNode(validatorAKey, qset);
        app = simulation->addNode(validatorBKey, qset);
        simulation->addNode(validatorCKey, qset);

        simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                         validatorCKey.getPublicKey());
        simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                         validatorBKey.getPublicKey());
        simulation->startAllNodes();

        // ValidatorB (with limits disabled) is the nomination leader
        auto lookup = [valBKey =
                           validatorBKey.getPublicKey()](NodeID const& n) {
            return (n == valBKey) ? 1000 : 1;
        };
        for (auto const& n : simulation->getNodes())
        {
            HerderImpl& herder = *static_cast<HerderImpl*>(&n->getHerder());
            herder.getHerderSCPDriver().setPriorityLookup(lookup);
        }
    };

    auto makeTxs = [&](Application::pointer app) {
        auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
        auto root = TestAccount::createRoot(*app);
        auto a1 = TestAccount{*app, getAccount("A")};
        auto b1 = TestAccount{*app, getAccount("B")};

        auto tx1 = root.tx({createAccount(a1, minBalance2)});
        auto tx2 = root.tx({createAccount(b1, minBalance2)});

        return std::make_tuple(root, a1, b1, tx1, tx2);
    };

    setup();

    auto [root, a1, b1, tx1, tx2] = makeTxs(app);

    // Submit txs for the same account, should be good
    REQUIRE(app->getHerder().recvTransaction(tx1, true).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

    // Second tx is rejected due to limit
    REQUIRE(app->getHerder().recvTransaction(tx2, true).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);

    uint32_t lcl = app->getLedgerManager().getLastClosedLedgerNum();
    simulation->crankUntil(
        [&]() {
            return app->getLedgerManager().getLastClosedLedgerNum() >= lcl + 2;
        },
        3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    for (auto const& node : simulation->getNodes())
    {
        // Applied txs were removed and banned
        REQUIRE(node->getHerder().getTx(tx1->getFullHash()) == nullptr);
        REQUIRE(node->getHerder().getTx(tx2->getFullHash()) == nullptr);
        REQUIRE(node->getHerder().isBannedTx(tx1->getFullHash()));
        // Second tx is not banned because it's never been flooded and
        // applied
        REQUIRE(!node->getHerder().isBannedTx(tx2->getFullHash()));
        // Only first account is in the ledger
        LedgerTxn ltx(node->getLedgerTxnRoot());
        REQUIRE(stellar::loadAccount(ltx, a1.getPublicKey()));
        REQUIRE(!stellar::loadAccount(ltx, b1.getPublicKey()));
    }

    // Now submit the second tx (which was rejected earlier) and make sure
    // it ends up in the ledger
    REQUIRE(app->getHerder().recvTransaction(tx2, true).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

    lcl = app->getLedgerManager().getLastClosedLedgerNum();
    simulation->crankUntil(
        [&]() {
            return app->getLedgerManager().getLastClosedLedgerNum() >= lcl + 2;
        },
        3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    for (auto const& node : simulation->getNodes())
    {
        // Applied tx was removed and banned
        REQUIRE(node->getHerder().getTx(tx2->getFullHash()) == nullptr);
        REQUIRE(node->getHerder().isBannedTx(tx2->getFullHash()));
        // Both accounts are in the ledger
        LedgerTxn ltx(node->getLedgerTxnRoot());
        REQUIRE(stellar::loadAccount(ltx, a1.getPublicKey()));
        REQUIRE(stellar::loadAccount(ltx, b1.getPublicKey()));
    }
}

TEST_CASE("soroban txs each parameter surge priced", "[soroban][herder]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    uint32_t baseTxRate = 1;
    uint32_t numAccounts = 100;
    auto test =
        [&](std::function<void(SorobanNetworkConfig & cfg)> tweakSorobanConfig,
            std::function<void(Config & appCfg)> tweakAppCfg) {
            auto simulation = Topologies::core(
                4, 1, Simulation::OVER_LOOPBACK, networkID, [&](int i) {
                    auto cfg = getTestConfig(i, Config::TESTDB_DEFAULT);
                    auto mid = std::numeric_limits<uint32_t>::max() / 2;
                    cfg.LOADGEN_INSTRUCTIONS_FOR_TESTING = {mid};
                    cfg.LOADGEN_INSTRUCTIONS_FOR_TESTING = {1};
                    cfg.LOADGEN_IO_KILOBYTES_FOR_TESTING = {60};
                    cfg.LOADGEN_IO_KILOBYTES_DISTRIBUTION_FOR_TESTING = {1};
                    cfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING = {256};
                    cfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING = {1};
                    cfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING = {mid};
                    cfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING = {1};
                    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 100;
                    tweakAppCfg(cfg);
                    return cfg;
                });
            simulation->startAllNodes();
            auto nodes = simulation->getNodes();
            upgradeSorobanNetworkConfig(
                [&tweakSorobanConfig](SorobanNetworkConfig& cfg) {
                    setSorobanNetworkConfigForTest(cfg);
                    auto mx = std::numeric_limits<uint32_t>::max();
                    // Set all Soroban resources to maximum initially; each
                    // section will adjust the config as desired
                    cfg.mLedgerMaxTxCount = mx;
                    cfg.mLedgerMaxInstructions = mx;
                    cfg.mLedgerMaxTransactionsSizeBytes = mx;
                    cfg.mLedgerMaxReadLedgerEntries = mx;
                    cfg.mLedgerMaxReadBytes = mx;
                    cfg.mLedgerMaxWriteLedgerEntries = mx;
                    cfg.mLedgerMaxWriteBytes = mx;
                    tweakSorobanConfig(cfg);
                },
                simulation);
            auto& loadGen = nodes[0]->getLoadGenerator();

            // Generate some accounts
            auto& loadGenDone = nodes[0]->getMetrics().NewMeter(
                {"loadgen", "run", "complete"}, "run");
            auto currLoadGenCount = loadGenDone.count();
            loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
                numAccounts, baseTxRate));
            simulation->crankUntil(
                [&]() { return loadGenDone.count() > currLoadGenCount; },
                10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

            // Setup invoke
            currLoadGenCount = loadGenDone.count();
            loadGen.generateLoad(
                GeneratedLoadConfig::createSorobanInvokeSetupLoad(
                    /* nAccounts */ numAccounts, /* nInstances */ 10,
                    /* txRate */ 1));
            simulation->crankUntil(
                [&]() { return loadGenDone.count() > currLoadGenCount; },
                100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

            auto& secondLoadGen = nodes[1]->getLoadGenerator();
            auto& secondLoadGenDone = nodes[1]->getMetrics().NewMeter(
                {"loadgen", "run", "complete"}, "run");
            // Generate load from several nodes, to produce both classic and
            // soroban traffic
            currLoadGenCount = loadGenDone.count();
            auto secondLoadGenCount = secondLoadGenDone.count();

            uint32_t maxInclusionFee = 100'000;
            auto sorobanConfig =
                GeneratedLoadConfig::txLoad(LoadGenMode::SOROBAN_INVOKE, 50,
                                            /* nTxs */ 100, baseTxRate * 3,
                                            /* offset */ 0, maxInclusionFee);

            // Ignore low fees, submit at a tx rate higher than the network
            // allows to trigger surge pricing
            sorobanConfig.skipLowFeeTxs = true;
            loadGen.generateLoad(sorobanConfig);

            // Generate Soroban txs from one node
            secondLoadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::PAY, 50,
                /* nTxs */ 50, baseTxRate, /* offset */ 50, maxInclusionFee));
            auto& loadGenFailed = nodes[0]->getMetrics().NewMeter(
                {"loadgen", "run", "failed"}, "run");
            auto& secondLoadGenFailed = nodes[1]->getMetrics().NewMeter(
                {"loadgen", "run", "failed"}, "run");
            bool hadSorobanSurgePricing = false;
            simulation->crankUntil(
                [&]() {
                    auto const& lclHeader = nodes[0]
                                                ->getLedgerManager()
                                                .getLastClosedLedgerHeader()
                                                .header;
                    auto txSet = nodes[0]->getHerder().getTxSet(
                        lclHeader.scpValue.txSetHash);
                    GeneralizedTransactionSet xdrTxSet;
                    txSet->toXDR(xdrTxSet);
                    auto const& phase = xdrTxSet.v1TxSet().phases.at(
                        static_cast<size_t>(TxSetPhase::SOROBAN));
                    std::optional<int64_t> baseFee;
                    switch (phase.v())
                    {
                    case 0:
                        if (!phase.v0Components().empty() &&
                            phase.v0Components()
                                .at(0)
                                .txsMaybeDiscountedFee()
                                .baseFee)
                        {

                            baseFee = *phase.v0Components()
                                           .at(0)
                                           .txsMaybeDiscountedFee()
                                           .baseFee;
                        }
                        break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                    case 1:
                        if (phase.parallelTxsComponent().baseFee)
                        {
                            baseFee = *phase.parallelTxsComponent().baseFee;
                        }
                        break;
#endif
                    default:
                        releaseAssert(false);
                    }

                    hadSorobanSurgePricing =
                        hadSorobanSurgePricing || (baseFee && *baseFee > 100);

                    return loadGenDone.count() > currLoadGenCount &&
                           secondLoadGenDone.count() > secondLoadGenCount;
                },
                200 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

            REQUIRE(loadGenFailed.count() == 0);
            REQUIRE(secondLoadGenFailed.count() == 0);
            REQUIRE(hadSorobanSurgePricing);
        };

    auto idTweakAppConfig = [](Config& cfg) { return cfg; };

    // We will be submitting soroban txs at desiredTxRate * 3, but the network
    // can only accept up to desiredTxRate for each resource dimension,
    // triggering surge pricing
    SECTION("operations")
    {
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = static_cast<uint32>(
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count());
        };
        test(tweakSorobanConfig, idTweakAppConfig);
    }
    SECTION("instructions")
    {
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxInstructions =
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() *
                cfg.mTxMaxInstructions;
        };
        auto tweakAppConfig = [](Config& cfg) {
            cfg.LOADGEN_INSTRUCTIONS_FOR_TESTING = {50'000'000};
        };
        test(tweakSorobanConfig, tweakAppConfig);
    }
    SECTION("tx size")
    {
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTransactionsSizeBytes = static_cast<uint32>(
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() *
                cfg.mTxMaxSizeBytes);
        };
        auto tweakAppConfig = [](Config& cfg) {
            cfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING = {60'000};
        };
        test(tweakSorobanConfig, tweakAppConfig);
    }
    SECTION("read entries")
    {
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxReadLedgerEntries = static_cast<uint32>(
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() *
                cfg.mTxMaxReadLedgerEntries);
        };
        auto tweakAppConfig = [](Config& cfg) {
            cfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING = {15};
        };
        test(tweakSorobanConfig, tweakAppConfig);
    }
    SECTION("write entries")
    {
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxWriteLedgerEntries = static_cast<uint32>(
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() *
                cfg.mTxMaxWriteLedgerEntries);
        };
        auto tweakAppConfig = [](Config& cfg) {
            cfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING = {15};
        };
        test(tweakSorobanConfig, tweakAppConfig);
    }
    SECTION("read bytes")
    {
        uint32_t constexpr txMaxReadBytes = 100 * 1024;
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mTxMaxReadBytes = txMaxReadBytes;
            cfg.mLedgerMaxReadBytes = static_cast<uint32>(
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() *
                cfg.mTxMaxReadBytes);
        };
        test(tweakSorobanConfig, idTweakAppConfig);
    }
    SECTION("write bytes")
    {
        auto tweakSorobanConfig = [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxWriteBytes = static_cast<uint32>(
                baseTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() *
                cfg.mTxMaxWriteBytes);
        };
        test(tweakSorobanConfig, idTweakAppConfig);
    }
}

TEST_CASE("overlay parallel processing")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);

    std::shared_ptr<Simulation> simulation;

    SECTION("background traffic processing")
    {
        // Set threshold to 1 so all have to vote
        simulation =
            Topologies::core(4, 1, Simulation::OVER_TCP, networkID, [](int i) {
                auto cfg = getTestConfig(i);
                cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 100;
                cfg.BACKGROUND_OVERLAY_PROCESSING = true;
                return cfg;
            });
    }

// Background ledger close requires postgres
#ifdef USE_POSTGRES
    SECTION("background ledger close")
    {
        // Set threshold to 1 so all have to vote
        simulation =
            Topologies::core(4, 1, Simulation::OVER_TCP, networkID, [](int i) {
                auto cfg = getTestConfig(i, Config::TESTDB_POSTGRESQL);
                cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 100;
                cfg.EXPERIMENTAL_PARALLEL_LEDGER_CLOSE = true;
                cfg.ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING =
                    std::chrono::milliseconds(500);
                return cfg;
            });
    }
#endif

    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    uint32_t desiredTxRate = 1;
    uint32_t ledgerWideLimit = static_cast<uint32>(
        desiredTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() * 2);
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            setSorobanNetworkConfigForTest(cfg);
            cfg.mLedgerMaxTxCount = ledgerWideLimit;
        },
        simulation);
    auto& loadGen = nodes[0]->getLoadGenerator();

    // Generate some accounts
    auto& loadGenDone =
        nodes[0]->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto currLoadGenCount = loadGenDone.count();
    uint32_t const numAccounts = 100;
    loadGen.generateLoad(
        GeneratedLoadConfig::createAccountsLoad(numAccounts, desiredTxRate));
    simulation->crankUntil(
        [&]() { return loadGenDone.count() > currLoadGenCount; },
        10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto& secondLoadGen = nodes[1]->getLoadGenerator();
    auto& secondLoadGenDone =
        nodes[1]->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    // Generate load from several nodes, to produce both classic and
    // soroban traffic
    currLoadGenCount = loadGenDone.count();
    auto secondLoadGenCount = secondLoadGenDone.count();
    uint32_t const txCount = 100;
    // Generate Soroban txs from one node
    loadGen.generateLoad(GeneratedLoadConfig::txLoad(
        LoadGenMode::SOROBAN_UPLOAD, 50,
        /* nTxs */ txCount, desiredTxRate, /* offset */ 0));
    // Generate classic txs from another node (with offset to prevent
    // overlapping accounts)
    secondLoadGen.generateLoad(GeneratedLoadConfig::txLoad(
        LoadGenMode::PAY, 50, txCount, desiredTxRate,
        /* offset */ 50));

    simulation->crankUntil(
        [&]() {
            return loadGenDone.count() > currLoadGenCount &&
                   secondLoadGenDone.count() > secondLoadGenCount;
        },
        200 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    auto& loadGenFailed =
        nodes[0]->getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");
    REQUIRE(loadGenFailed.count() == 0);
    auto& secondLoadGenFailed =
        nodes[1]->getMetrics().NewMeter({"loadgen", "run", "failed"}, "run");
    REQUIRE(secondLoadGenFailed.count() == 0);
}

TEST_CASE("soroban txs accepted by the network",
          "[herder][soroban][transactionqueue]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);

    // Set threshold to 1 so all have to vote
    auto simulation =
        Topologies::core(4, 1, Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i, Config::TESTDB_DEFAULT);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 100;
            return cfg;
        });
    simulation->startAllNodes();
    auto nodes = simulation->getNodes();
    uint32_t desiredTxRate = 1;
    uint32_t ledgerWideLimit = static_cast<uint32>(
        desiredTxRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count() * 2);
    uint32_t const numAccounts = 100;
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            setSorobanNetworkConfigForTest(cfg);
            cfg.mLedgerMaxTxCount = ledgerWideLimit;
        },
        simulation);

    auto& loadGen = nodes[0]->getLoadGenerator();
    auto& txsSucceeded =
        nodes[0]->getMetrics().NewCounter({"ledger", "apply", "success"});
    auto& txsFailed =
        nodes[0]->getMetrics().NewCounter({"ledger", "apply", "failure"});
    auto& sorobanTxsSucceeded = nodes[0]->getMetrics().NewCounter(
        {"ledger", "apply-soroban", "success"});
    auto& sorobanTxsFailed = nodes[0]->getMetrics().NewCounter(
        {"ledger", "apply-soroban", "failure"});

    // Generate some accounts
    auto& loadGenDone =
        nodes[0]->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto currLoadGenCount = loadGenDone.count();
    loadGen.generateLoad(
        GeneratedLoadConfig::createAccountsLoad(numAccounts, desiredTxRate));
    simulation->crankUntil(
        [&]() { return loadGenDone.count() > currLoadGenCount; },
        10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    uint64_t lastSorobanSucceeded = sorobanTxsSucceeded.count();
    uint64_t lastSucceeded = txsSucceeded.count();
    REQUIRE(lastSucceeded > 0);
    REQUIRE(txsFailed.count() == 0);

    SECTION("soroban only")
    {
        currLoadGenCount = loadGenDone.count();
        auto uploadCfg = GeneratedLoadConfig::txLoad(
            LoadGenMode::SOROBAN_UPLOAD, numAccounts,
            /* nTxs */ 100, desiredTxRate, /*offset*/ 0);

        // Make sure that a significant fraction of some soroban txs get
        // applied (some may fail due to exceeding the declared resource
        // limits or due to XDR parsing errors).
        uploadCfg.setMinSorobanPercentSuccess(50);

        // Now generate soroban txs.
        loadGen.generateLoad(uploadCfg);

        simulation->crankUntil(
            [&]() { return loadGenDone.count() > currLoadGenCount; },
            50 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        auto& loadGenFailed = nodes[0]->getMetrics().NewMeter(
            {"loadgen", "run", "failed"}, "run");
        REQUIRE(loadGenFailed.count() == 0);

        SECTION("upgrade max soroban tx set size")
        {
            // Ensure more transactions get in the ledger post upgrade
            ConfigUpgradeSetFrameConstPtr res;
            Upgrades::UpgradeParameters scheduledUpgrades;
            auto lclCloseTime =
                VirtualClock::from_time_t(nodes[0]
                                              ->getLedgerManager()
                                              .getLastClosedLedgerHeader()
                                              .header.scpValue.closeTime);
            scheduledUpgrades.mUpgradeTime = lclCloseTime;
            scheduledUpgrades.mMaxSorobanTxSetSize = ledgerWideLimit * 10;
            for (auto const& app : nodes)
            {
                app->getHerder().setUpgrades(scheduledUpgrades);
            }

            // Ensure upgrades went through
            simulation->crankForAtLeast(std::chrono::seconds(20), false);
            for (auto node : nodes)
            {
                REQUIRE(node->getLedgerManager()
                            .getSorobanNetworkConfigReadOnly()
                            .ledgerMaxTxCount() == ledgerWideLimit * 10);
            }

            currLoadGenCount = loadGenDone.count();
            // Now generate soroban txs.
            auto loadCfg = GeneratedLoadConfig::txLoad(
                LoadGenMode::SOROBAN_UPLOAD, numAccounts,
                /* nTxs */ 100, desiredTxRate * 5, /*offset*/ 0);
            loadCfg.skipLowFeeTxs = true;
            // Make sure some soroban txs get applied.
            loadCfg.setMinSorobanPercentSuccess(50);
            loadGen.generateLoad(loadCfg);

            bool upgradeApplied = false;
            simulation->crankUntil(
                [&]() {
                    auto txSetSize =
                        nodes[0]
                            ->getHerder()
                            .getTxSet(nodes[0]
                                          ->getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header.scpValue.txSetHash)
                            ->sizeOpTotalForLogging();
                    upgradeApplied =
                        upgradeApplied || txSetSize > ledgerWideLimit;
                    return loadGenDone.count() > currLoadGenCount;
                },
                10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
            REQUIRE(loadGenFailed.count() == 0);
            REQUIRE(upgradeApplied);
        }
    }
    SECTION("soroban and classic")
    {
        auto& secondLoadGen = nodes[1]->getLoadGenerator();
        auto& secondLoadGenDone = nodes[1]->getMetrics().NewMeter(
            {"loadgen", "run", "complete"}, "run");
        // Generate load from several nodes, to produce both classic and
        // soroban traffic
        currLoadGenCount = loadGenDone.count();
        auto secondLoadGenCount = secondLoadGenDone.count();
        uint32_t const classicTxCount = 500;
        SECTION("basic load")
        {
            // Generate Soroban txs from one node
            loadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::SOROBAN_UPLOAD, 50,
                /* nTxs */ 500, desiredTxRate, /* offset */ 0));
            // Generate classic txs from another node (with offset to prevent
            // overlapping accounts)
            secondLoadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::PAY, 50, classicTxCount, desiredTxRate,
                /* offset */ 50));
        }
        SECTION("soroban surge pricing")
        {
            uint32_t maxInclusionFee = 100'000;
            auto sorobanConfig =
                GeneratedLoadConfig::txLoad(LoadGenMode::SOROBAN_UPLOAD, 50,
                                            /* nTxs */ 500, desiredTxRate * 3,
                                            /* offset */ 0, maxInclusionFee);

            // Make sure some soroban txs get applied.
            sorobanConfig.setMinSorobanPercentSuccess(40);

            // Ignore low fees, submit at a tx rate higher than the network
            // allows to trigger surge pricing
            sorobanConfig.skipLowFeeTxs = true;
            loadGen.generateLoad(sorobanConfig);
            // Generate a lot of classic txs from one node
            secondLoadGen.generateLoad(GeneratedLoadConfig::txLoad(
                LoadGenMode::PAY, 50, classicTxCount, desiredTxRate,
                /* offset */ 50, maxInclusionFee));
        }

        simulation->crankUntil(
            [&]() {
                return loadGenDone.count() > currLoadGenCount &&
                       secondLoadGenDone.count() > secondLoadGenCount;
            },
            200 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        auto& loadGenFailed = nodes[0]->getMetrics().NewMeter(
            {"loadgen", "run", "failed"}, "run");
        REQUIRE(loadGenFailed.count() == 0);
        auto& secondLoadGenFailed = nodes[1]->getMetrics().NewMeter(
            {"loadgen", "run", "failed"}, "run");
        REQUIRE(secondLoadGenFailed.count() == 0);
        // Check all classic txs got applied
        REQUIRE(txsSucceeded.count() - lastSucceeded -
                    sorobanTxsSucceeded.count() +
                    lastSorobanSucceeded /* to prevent double counting */
                == classicTxCount);
        REQUIRE(txsFailed.count() == sorobanTxsFailed.count());
    }
}

static void
checkSynced(Application& app)
{
    REQUIRE(app.getLedgerManager().isSynced());
    REQUIRE(!app.getLedgerApplyManager().maybeGetNextBufferedLedgerToApply());
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
static void
herderExternalizesValuesWithProtocol(uint32_t version,
                                     bool parallelLedgerClose = false,
                                     uint32_t delayCloseMs = 0)
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = std::make_shared<Simulation>(
        Simulation::OVER_LOOPBACK, networkID, [&](int i) {
            Config::TestDbMode dbMode = Config::TESTDB_BUCKET_DB_PERSISTENT;
            if (parallelLedgerClose)
            {
#ifdef USE_POSTGRES
                dbMode = Config::TESTDB_POSTGRESQL;
#else
                FAIL("Parallel ledger close requires postgres");
#endif
            }
            auto cfg = getTestConfig(i, dbMode);
            cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = version;
            if (parallelLedgerClose)
            {
                cfg.EXPERIMENTAL_PARALLEL_LEDGER_CLOSE = true;
                // Add artifical delay to ledger close to increase chances of
                // conflicts
                cfg.ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING =
                    std::chrono::milliseconds(delayCloseMs);
            }
            return cfg;
        });

    auto validatorAKey = SecretKey::fromSeed(sha256("validator-A"));
    auto validatorBKey = SecretKey::fromSeed(sha256("validator-B"));
    auto validatorCKey = SecretKey::fromSeed(sha256("validator-C"));

    SCPQuorumSet qset;
    qset.threshold = 2;
    qset.validators.push_back(validatorAKey.getPublicKey());
    qset.validators.push_back(validatorBKey.getPublicKey());
    qset.validators.push_back(validatorCKey.getPublicKey());

    auto A = simulation->addNode(validatorAKey, qset);
    auto B = simulation->addNode(validatorBKey, qset);
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
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            cfg.mStateArchivalSettings.bucketListWindowSamplePeriod = 1;
        },
        simulation);

    // After SCP is restored, Herder is tracking
    REQUIRE(getC()->getHerder().getState() ==
            Herder::State::HERDER_TRACKING_NETWORK_STATE);

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
            10 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        return std::min(currentALedger(), currentCLedger());
    };

    auto reconnectAndCloseLedgers = [&](uint32_t numLedgers) {
        simulation->addConnection(validatorAKey.getPublicKey(),
                                  validatorBKey.getPublicKey());
        simulation->addConnection(validatorAKey.getPublicKey(),
                                  validatorCKey.getPublicKey());
        return waitForLedgers(numLedgers);
    };

    HerderImpl& herderA = *static_cast<HerderImpl*>(&A->getHerder());
    HerderImpl& herderB = *static_cast<HerderImpl*>(&B->getHerder());
    HerderImpl& herderC = *static_cast<HerderImpl*>(&getC()->getHerder());
    auto const& lmC = getC()->getLedgerManager();

    auto waitForAB = [&](int nLedgers, bool waitForB) {
        auto destinationLedger = currentALedger() + nLedgers;
        bool submitted = false;
        simulation->crankUntil(
            [&]() {
                if (currentALedger() == (destinationLedger - 1) && !submitted)
                {
                    auto root = TestAccount::createRoot(*A);
                    SorobanResources resources;
                    auto sorobanTx = createUploadWasmTx(
                        *A, root, 100, DEFAULT_TEST_RESOURCE_FEE, resources);
                    REQUIRE(
                        herderA.recvTransaction(sorobanTx, true).code ==
                        TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                    submitted = true;
                }
                return currentALedger() >= destinationLedger &&
                       (!waitForB || currentBLedger() >= destinationLedger);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        return currentALedger();
    };

    uint32_t currentLedger = currentBLedger();
    REQUIRE(currentALedger() == currentLedger);
    REQUIRE(currentCLedger() == currentLedger);

    // All nodes externalize a few ledgers
    auto fewLedgers = A->getConfig().MAX_SLOTS_TO_REMEMBER / 2;
    currentLedger = waitForLedgers(fewLedgers);

    // C is at most a ledger behind
    REQUIRE(currentALedger() >= currentLedger);
    REQUIRE(currentCLedger() == currentLedger);

    // Arm the upgrade, but don't close the upgrade ledger yet
    // C won't upgrade until it's on the right LCL
    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTransactionsSizeBytes = 1'000'000;
            cfg.mTxMaxSizeBytes = 500'000;
        },
        simulation, /*applyUpgrade=*/false);

    // disconnect C
    simulation->dropConnection(validatorAKey.getPublicKey(),
                               validatorCKey.getPublicKey());

    currentLedger = currentALedger();

    // Advance A and B a bit further, and collect externalize messages
    std::map<uint32_t, std::pair<SCPEnvelope, StellarMessage>>
        validatorSCPMessagesA;
    std::map<uint32_t, std::pair<SCPEnvelope, StellarMessage>>
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
                validatorSCPMessagesA[start] =
                    std::make_pair(env, txset->toStellarMessage());
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
                validatorSCPMessagesB[start] =
                    std::make_pair(env, txset->toStellarMessage());
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
        simulation->crankForAtLeast(std::chrono::seconds(10), false);
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
        // This should trigger LedgerApplyManager to start buffering ledgers
        // Ensure C processes future tx set and its fees correctly (even though
        // its own ledger state isn't upgraded yet)
        receiveLedger(fourth, herderC);
        if (protocolVersionStartsFrom(version, SOROBAN_PROTOCOL_VERSION))
        {
            REQUIRE(herderA.getMaxTxSize() ==
                    500'000 + herderA.getFlowControlExtraBuffer());
            REQUIRE(herderB.getMaxTxSize() ==
                    500'000 + herderB.getFlowControlExtraBuffer());
            REQUIRE(herderC.getMaxTxSize() < herderA.getMaxTxSize());
        }

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
        // LedgerApplyManager should apply buffered ledgers and let LM get back
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
            REQUIRE(!getC()->getLedgerApplyManager().isCatchupInitialized());

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

        // C properly upgraded max tx size despite externalizing out-of-order
        if (protocolVersionStartsFrom(version, SOROBAN_PROTOCOL_VERSION))
        {
            REQUIRE(herderC.getMaxTxSize() ==
                    500'000 + herderC.getFlowControlExtraBuffer());
        }

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

        // C landed on the same hash as A and B
        REQUIRE(A->getLedgerManager().getLastClosedLedgerHeader().hash ==
                getC()->getLedgerManager().getLastClosedLedgerHeader().hash);
        REQUIRE(B->getLedgerManager().getLastClosedLedgerHeader().hash ==
                getC()->getLedgerManager().getLastClosedLedgerHeader().hash);
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

        // Make sure nodes continue closing ledgers normally
        reconnectAndCloseLedgers(fewLedgers);
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
        reconnectAndCloseLedgers(fewLedgers);
    }

    SECTION("older ledgers externalize and no-op")
    {
        // Reconnect nodes to crank the simulation just enough to purge older
        // slots
        auto configC = getC()->getConfig();
        auto currentlyTracking =
            reconnectAndCloseLedgers(configC.MAX_SLOTS_TO_REMEMBER + 1);

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

        // Make sure nodes continue closing ledgers normally despite old data
        reconnectAndCloseLedgers(fewLedgers);
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

TEST_CASE("herder externalizes values", "[herder]")
{
    SECTION("prev protocol")
    {
        herderExternalizesValuesWithProtocol(
            Config::CURRENT_LEDGER_PROTOCOL_VERSION - 1);
    }
    SECTION("curr protocol")
    {
        herderExternalizesValuesWithProtocol(
            Config::CURRENT_LEDGER_PROTOCOL_VERSION);
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
        return getTestConfig(i, Config::TESTDB_DEFAULT);
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

    auto classicTxs = txs;

    TxFrameList sorobanTxs;
    for (auto it = classicTxs.begin(); it != classicTxs.end();)
    {
        if ((*it)->isSoroban())
        {
            sorobanTxs.emplace_back(*it);
            it = classicTxs.erase(it);
        }
        else
        {
            ++it;
        }
    }

    PerPhaseTransactionList txsPhases{classicTxs};

    txsPhases.emplace_back(sorobanTxs);

    auto [txSet, applicableTxSet] =
        makeTxSetFromTransactions(txsPhases, app, 0, 0);
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
    cfg.ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING = std::chrono::seconds(0);
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
    auto [_, applicableTxSet] = makeTxSetFromTransactions(txs, *app, 0, 0);
    REQUIRE(applicableTxSet->sizeTxTotal() == 1);
    REQUIRE((*applicableTxSet->getPhase(TxSetPhase::CLASSIC).begin())
                ->getContentsHash() == tx1a->getContentsHash());
    REQUIRE(applicableTxSet->checkValid(*app, 0, 0));
}

TEST_CASE("do not flood too many soroban transactions",
          "[soroban][herder][transactionqueue]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = std::make_shared<Simulation>(
        Simulation::OVER_LOOPBACK, networkID, [&](int i) {
            auto cfg = getTestConfig(i);
            cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
            cfg.NODE_IS_VALIDATOR = true;
            cfg.FORCE_SCP = true;
            cfg.FLOOD_TX_PERIOD_MS = 100;
            cfg.FLOOD_OP_RATE_PER_LEDGER = 2.0;
            cfg.FLOOD_SOROBAN_TX_PERIOD_MS = 50;
            cfg.FLOOD_SOROBAN_RATE_PER_LEDGER = 2.0;
            cfg.ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING =
                std::chrono::seconds(0);
            return cfg;
        });

    auto mainKey = SecretKey::fromSeed(sha256("main"));
    auto otherKey = SecretKey::fromSeed(sha256("other"));

    SCPQuorumSet qset;
    qset.threshold = 2;
    qset.validators.push_back(mainKey.getPublicKey());
    qset.validators.push_back(otherKey.getPublicKey());

    simulation->addNode(mainKey, qset);
    simulation->addNode(otherKey, qset);

    auto app = simulation->getNode(mainKey.getPublicKey());

    simulation->addPendingConnection(mainKey.getPublicKey(),
                                     otherKey.getPublicKey());
    simulation->startAllNodes();
    simulation->crankForAtLeast(std::chrono::seconds(1), false);

    upgradeSorobanNetworkConfig(
        [&](SorobanNetworkConfig& cfg) {
            setSorobanNetworkConfigForTest(cfg);
            // Update read entries to allow flooding at most 1 tx per broadcast
            // interval.
            cfg.mLedgerMaxReadLedgerEntries = 40;
            cfg.mLedgerMaxReadBytes = cfg.mTxMaxReadBytes;
        },
        simulation);

    auto const& cfg = app->getConfig();
    auto& lm = app->getLedgerManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    auto& tq = herder.getSorobanTransactionQueue();

    auto root = TestAccount::createRoot(*app);
    std::vector<TestAccount> accs;

    // number of accounts to use
    // About 2x ledgers worth of soroban txs (configured below)
    int const nbAccounts = 39;

    uint32 curFeeOffset = 10000;

    accs.reserve(nbAccounts);
    for (int i = 0; i < nbAccounts; ++i)
    {
        accs.emplace_back(
            root.create(fmt::format("A{}", i), lm.getLastMinBalance(2)));
    }
    std::deque<uint32> inclusionFees;

    uint32_t const baseInclusionFee = 100'000;
    SorobanResources resources;
    resources.instructions = 800'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;

    auto genTx = [&](TestAccount& source, bool highFee) {
        auto inclusionFee = baseInclusionFee;
        if (highFee)
        {
            inclusionFee += 1'000'000;
            inclusionFees.emplace_front(inclusionFee);
        }
        else
        {
            inclusionFee += curFeeOffset;
            inclusionFees.emplace_back(inclusionFee);
        }
        curFeeOffset--;

        auto tx = createUploadWasmTx(*app, source, inclusionFee, 10'000'000,
                                     resources);
        REQUIRE(herder.recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        return tx;
    };

    auto tx1a = genTx(accs[0], false);
    auto tx1r = genTx(root, false);
    int numTx = 2;
    for (int i = 1; i < accs.size(); i++)
    {
        genTx(accs[i], false);
        numTx++;
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
        REQUIRE(tx->getInclusionFee() == inclusionFees.front());
        inclusionFees.pop_front();
        ++numBroadcast;
    };

    REQUIRE(tq.getTransactions({}).size() == numTx);

    // remove the first two transactions that won't be
    // re-broadcasted during externalize
    inclusionFees.pop_front();
    inclusionFees.pop_front();

    externalize(cfg.NODE_SEED, lm, herder, {tx1a, tx1r}, *app);
    REQUIRE(tq.getTransactions({}).size() == numTx - 2);

    SECTION("txs properly spaced out")
    {
        // no broadcast right away
        REQUIRE(numBroadcast == 0);
        tq.clearBroadcastCarryover();

        // wait for a bit more than a broadcast period
        // rate per period is 100 ms
        auto broadcastPeriod =
            std::chrono::milliseconds(cfg.FLOOD_SOROBAN_TX_PERIOD_MS);
        auto const delta = std::chrono::milliseconds(1);
        simulation->crankForAtLeast(broadcastPeriod + delta, false);

        // Could broadcast exactly 1 txs
        REQUIRE(numBroadcast == 1);
        REQUIRE(tq.getTransactions({}).size() == numTx - 2);

        // Submit an expensive tx that will be broadcasted before cheaper ones
        simulation->crankForAtLeast(std::chrono::milliseconds(500), false);
        genTx(root, true);

        // Wait half a ledger to flood _at least_ 1 ledger worth of traffic
        simulation->crankForAtLeast(std::chrono::milliseconds(2000), false);
        REQUIRE(numBroadcast >= std::ceil((numTx - 1) / 2));
        REQUIRE(tq.getTransactions({}).size() == numTx - 1);

        // Crank for another half ledger, should broadcast everything at this
        // point
        simulation->crankForAtLeast(std::chrono::milliseconds(2500), false);
        REQUIRE(numBroadcast == numTx - 1);
        REQUIRE(tq.getTransactions({}).size() == numTx - 1);
        simulation->stopAllNodes();
    }
    SECTION("large tx waits to accumulate enough quota")
    {
        REQUIRE(numBroadcast == 0);
        // For large txs, there might not be enough resources allocated for
        // this flooding period. In this case, wait a few periods to accumulate
        // enough quota
        resources.readBytes = 200 * 1024;

        genTx(root, true);
        simulation->crankForAtLeast(std::chrono::milliseconds(2000), false);
        REQUIRE(numBroadcast == 0);
        simulation->crankForAtLeast(std::chrono::milliseconds(1000), false);
        REQUIRE(numBroadcast >= 1);
    }
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
        size_t const maxOps = cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE;
        int const nbAccounts = static_cast<int>(maxOps);
        // number of transactions to generate per fee
        // groups are
        int const feeGroupMaxSize = 7;
        // used to track fee
        int feeGroupSize = 0;
        uint32 curFeeOffset = 10000;

        accs.reserve(nbAccounts);
        accs.emplace_back(root);
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
            auto txFee = static_cast<uint32_t>(tx->getFullFee());
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
            setFullFee(tx, txFee);
            getSignatures(tx).clear();
            tx->addSignature(source.getSecretKey());
            if (++feeGroupSize == feeGroupMaxSize)
            {
                feeGroupSize = 0;
                curFeeOffset--;
            }

            REQUIRE(herder.recvTransaction(tx, false).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            return tx;
        };

        auto nextAccountIt = accs.begin();
        auto getNextAccountTx = [&](uint32_t numOps, bool highFee = false) {
            REQUIRE(nextAccountIt != accs.end());
            auto tx = genTx(*nextAccountIt, numOps, highFee);
            nextAccountIt++;
            return tx;
        };

        auto tx1a = getNextAccountTx(numOps);
        auto tx1r = getNextAccountTx(numOps);
        size_t numTx = 2;
        for (; (numTx + 2) * numOps <= maxOps; ++numTx)
        {
            getNextAccountTx(numOps);
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
            REQUIRE(tx->getFullFee() == fees.front());
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
        getNextAccountTx(numOps, /* highFee */ true);

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
        int const nbAccounts =
            app->getConfig().TESTING_UPGRADE_MAX_TX_SET_SIZE * 2;
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
            auto txFee = tx->getFullFee();
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
            REQUIRE(txFee <= std::numeric_limits<uint32>::max());
            setFullFee(tx, static_cast<uint32>(txFee));
            getSignatures(tx).clear();
            tx->addSignature(source.getSecretKey());
            if (++feeGroupSize == feeGroupMaxSize)
            {
                feeGroupSize = 0;
                curFeeOffset--;
            }

            REQUIRE(herder.recvTransaction(tx, false).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            return tx;
        };

        auto nextAccountIdx = 0;
        auto genNextAccountTx = [&](bool isDex, uint32_t numOps,
                                    bool highFee = false) {
            REQUIRE(nextAccountIdx < accs.size());
            return genTx(nextAccountIdx++, isDex, numOps, highFee);
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
                    genNextAccountTx(true, opsPerDexTx);
                    ++generatedDex;
                }
                else
                {
                    genNextAccountTx(false, opsPerNonDexTx);
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
                    genNextAccountTx(true, opsPerDexTx);
                }
            }
            for (uint32_t i = 0; i < nonDexTxs; ++i)
            {
                genNextAccountTx(false, opsPerNonDexTx);
            }
            if (!broadcastDexFirst)
            {
                for (uint32_t i = 0; i < dexTxs; ++i)
                {
                    genNextAccountTx(true, opsPerDexTx);
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

            REQUIRE(tx->getFullFee() == expectedFee);
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
        auto constexpr dexOpsRatePerPeriod = 8u;
        auto const broadcastPeriod =
            std::chrono::milliseconds(cfg.FLOOD_TX_PERIOD_MS);
        auto const delta = std::chrono::milliseconds(1);
        int noBroadcastPeriods = 0;

        // Make 50(=5s/100ms) broadcast 'iterations' by cranking timer for
        // broadcastPeriod.
        for (uint32_t broadcastIter = 0; broadcastIter < 50; ++broadcastIter)
        {
            // Inject new transactions from unused account in the middle of
            // ledger period.
            if (broadcastIter == 25)
            {
                if (dexTxs > 0)
                {
                    ++dexTxs;
                    genNextAccountTx(true, opsPerDexTx, true);
                }
                if (nonDexTxs > 0)
                {
                    ++nonDexTxs;
                    genNextAccountTx(false, opsPerNonDexTx, true);
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
        TxSetXDRFrameConstPtr txSet = TxSetXDRFrame::makeEmpty(
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

        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
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

        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_FILTERED);
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

        REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
    }
}

// Test that Herder updates the scphistory table with additional messages from
// ledger `n-1` when closing ledger `n`
TEST_CASE("SCP message capture from previous ledger", "[herder]")
{
    // Initialize simulation
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    // Create three validators: A, B, and C
    auto validatorAKey = SecretKey::fromSeed(sha256("validator-A"));
    auto validatorBKey = SecretKey::fromSeed(sha256("validator-B"));
    auto validatorCKey = SecretKey::fromSeed(sha256("validator-C"));

    // Put all validators in a quorum set of threshold 2
    SCPQuorumSet qset;
    qset.threshold = 2;
    qset.validators.push_back(validatorAKey.getPublicKey());
    qset.validators.push_back(validatorBKey.getPublicKey());
    qset.validators.push_back(validatorCKey.getPublicKey());

    // Connect validators A and B, but leave C disconnected
    auto A = simulation->addNode(validatorAKey, qset);
    auto B = simulation->addNode(validatorBKey, qset);
    auto C = simulation->addNode(validatorCKey, qset);
    simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                     validatorBKey.getPublicKey());
    simulation->startAllNodes();

    // Crank A and B until they're on ledger 2
    simulation->crankUntil(
        [&]() {
            return A->getLedgerManager().getLastClosedLedgerNum() == 2 &&
                   B->getLedgerManager().getLastClosedLedgerNum() == 2;
        },
        4 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Check that a node's scphistory table for a given ledger has the correct
    // number of entries of each type in `expectedTypes`
    auto checkSCPHistoryEntries =
        [&](Application::pointer node, uint32_t ledgerNum,
            UnorderedMap<SCPStatementType, size_t> const& expectedTypes) {
            // Prepare query
            auto& db = node->getDatabase();
            auto prep = db.getPreparedStatement(
                "SELECT envelope FROM scphistory WHERE ledgerseq = :l",
                db.getSession());
            auto& st = prep.statement();
            st.exchange(soci::use(ledgerNum));
            std::string envStr;
            st.exchange(soci::into(envStr));
            st.define_and_bind();
            st.execute(false);

            // Count the number of entries of each type
            UnorderedMap<SCPStatementType, size_t> actualTypes;
            while (st.fetch())
            {
                Value v;
                decoder::decode_b64(envStr, v);
                SCPEnvelope env;
                xdr::xdr_from_opaque(v, env);
                ++actualTypes[env.statement.pledges.type()];
            }

            return actualTypes == expectedTypes;
        };

    // Expected counts of scphistory entry types for ledger 2
    UnorderedMap<SCPStatementType, size_t> expConfExt = {
        {SCPStatementType::SCP_ST_CONFIRM, 1},
        {SCPStatementType::SCP_ST_EXTERNALIZE, 1}};
    UnorderedMap<SCPStatementType, size_t> exp2Ext = {
        {SCPStatementType::SCP_ST_EXTERNALIZE, 2}};

    // Examine scphistory tables for A and B for ledger 2. Either A has 1
    // CONFIRM and 1 EXTERNALIZE and B has 2 EXTERNALIZEs, or A has 2
    // EXTERNALIZEs and B has 1 CONFIRM and 1 EXTERNALIZE.
    REQUIRE((checkSCPHistoryEntries(A, 2, expConfExt) &&
             checkSCPHistoryEntries(B, 2, exp2Ext)) ^
            (checkSCPHistoryEntries(A, 2, exp2Ext) &&
             checkSCPHistoryEntries(B, 2, expConfExt)));

    // C has no entries in its scphistory table for ledger 2.
    REQUIRE(checkSCPHistoryEntries(C, 2, {}));

    // Get messages from A and B
    HerderImpl& herderA = dynamic_cast<HerderImpl&>(A->getHerder());
    HerderImpl& herderB = dynamic_cast<HerderImpl&>(B->getHerder());
    std::vector<SCPEnvelope> AEnvs = herderA.getSCP().getLatestMessagesSend(2);
    std::vector<SCPEnvelope> BEnvs = herderB.getSCP().getLatestMessagesSend(2);

    // Pass A and B's messages to C
    for (auto const& env : AEnvs)
    {
        C->getHerder().recvSCPEnvelope(env);
    }
    for (auto const& env : BEnvs)
    {
        C->getHerder().recvSCPEnvelope(env);
    }

    // Crank C until it is on ledger 2
    simulation->crankUntil(
        [&]() { return C->getLedgerManager().getLastClosedLedgerNum() == 2; },
        4 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Get messages from C
    HerderImpl& herderC = dynamic_cast<HerderImpl&>(C->getHerder());
    std::vector<SCPEnvelope> CEnvs = herderC.getSCP().getLatestMessagesSend(2);

    // Pass C's messages to A and B
    for (auto const& env : CEnvs)
    {
        A->getHerder().recvSCPEnvelope(env);
        B->getHerder().recvSCPEnvelope(env);
    }

    // Crank A and B until they're on ledger 3
    simulation->crankUntil(
        [&]() {
            return A->getLedgerManager().getLastClosedLedgerNum() == 3 &&
                   B->getLedgerManager().getLastClosedLedgerNum() == 3;
        },
        4 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // A and B should now each have 3 EXTERNALIZEs in their scphistory table for
    // ledger 2. A's CONFIRM entry has been replaced with an EXTERNALIZE.
    UnorderedMap<SCPStatementType, size_t> const expectedTypes = {
        {SCPStatementType::SCP_ST_EXTERNALIZE, 3}};
    REQUIRE(checkSCPHistoryEntries(A, 2, expectedTypes));
    REQUIRE(checkSCPHistoryEntries(B, 2, expectedTypes));

    // Connect C to B and crank C to catch up with A and B
    simulation->addConnection(validatorCKey.getPublicKey(),
                              validatorBKey.getPublicKey());
    simulation->crankUntil(
        [&]() { return C->getLedgerManager().getLastClosedLedgerNum() >= 3; },
        4 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // C should have 3 EXTERNALIZEs in its scphistory table for ledger 2. This
    // check ensures that C does not double count messages from ledger 2 when
    // closing ledger 3.
    REQUIRE(checkSCPHistoryEntries(C, 2, expectedTypes));
}

using Topology = std::pair<std::vector<SecretKey>, std::vector<ValidatorEntry>>;

// Generate a Topology with a single org containing 3 validators of HIGH quality
static Topology
simpleThreeNode()
{
    // Generate validators
    std::vector<SecretKey> sks;
    std::vector<ValidatorEntry> validators;
    int constexpr numValidators = 3;
    for (int i = 0; i < numValidators; ++i)
    {
        SecretKey const& key = sks.emplace_back(SecretKey::random());
        ValidatorEntry& entry = validators.emplace_back();
        entry.mName = fmt::format("validator-{}", i);
        entry.mHomeDomain = "A";
        entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
        entry.mKey = key.getPublicKey();
        entry.mHasHistory = false;
    }
    return {sks, validators};
}

// Generate a topology with 3 orgs of HIGH quality. Two orgs have 3 validators
// and one org has 5 validators.
static Topology
unbalancedOrgs()
{
    // Generate validators
    std::vector<SecretKey> sks;
    std::vector<ValidatorEntry> validators;
    int constexpr numValidators = 11;
    for (int i = 0; i < numValidators; ++i)
    {
        // Orgs A and B have 3 validators each. Org C has 5 validators.
        std::string org = "C";
        if (i < 3)
        {
            org = "A";
        }
        else if (i < 6)
        {
            org = "B";
        }

        SecretKey const& key = sks.emplace_back(SecretKey::random());
        ValidatorEntry& entry = validators.emplace_back();
        entry.mName = fmt::format("validator-{}", i);
        entry.mHomeDomain = org;
        entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
        entry.mKey = key.getPublicKey();
        entry.mHasHistory = false;
    }
    return {sks, validators};
}

// Generate a tier1-like topology. This topology has 7 HIGH quality orgs, 6 of
// which have 3 validators and 1 has 5 validators.
static Topology
teir1Like()
{
    std::vector<SecretKey> sks;
    std::vector<ValidatorEntry> validators;
    int constexpr numOrgs = 7;

    for (int i = 0; i < numOrgs; ++i)
    {
        std::string const org = fmt::format("org-{}", i);
        int const numValidators = i == 0 ? 5 : 3;
        for (int j = 0; j < numValidators; ++j)
        {
            SecretKey const& key = sks.emplace_back(SecretKey::random());
            ValidatorEntry& entry = validators.emplace_back();
            entry.mName = fmt::format("validator-{}-{}", i, j);
            entry.mHomeDomain = org;
            entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
            entry.mKey = key.getPublicKey();
            entry.mHasHistory = false;
        }
    }

    return {sks, validators};
}

// Returns a random quality up to `maxQuality`
static ValidatorQuality
randomQuality(ValidatorQuality maxQuality)
{
    return static_cast<ValidatorQuality>(rand_uniform<int>(
        static_cast<int>(ValidatorQuality::VALIDATOR_LOW_QUALITY),
        static_cast<int>(maxQuality)));
}

// Returns the minimum size an org of quality `q` can have
static int constexpr minOrgSize(ValidatorQuality q)
{
    switch (q)
    {
    case ValidatorQuality::VALIDATOR_LOW_QUALITY:
    case ValidatorQuality::VALIDATOR_MED_QUALITY:
        return 1;
    case ValidatorQuality::VALIDATOR_HIGH_QUALITY:
    case ValidatorQuality::VALIDATOR_CRITICAL_QUALITY:
        return 3;
    }
}

// Generate a random topology with up to `maxValidators` validators. Ensures at
// least one org is HIGH quality.
static Topology
randomTopology(int maxValidators)
{
    int const numValidators = rand_uniform<int>(3, maxValidators);
    int constexpr minCritOrgSize =
        minOrgSize(ValidatorQuality::VALIDATOR_CRITICAL_QUALITY);

    // Generate validators
    int curOrg = 0;
    int curOrgSize = 0;
    ValidatorQuality curQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
    std::vector<SecretKey> sks(numValidators);
    std::vector<ValidatorEntry> validators(numValidators);
    for (int i = 0; i < numValidators; ++i)
    {
        if (curOrgSize >= minOrgSize(curQuality) && rand_flip())
        {
            // Start new org
            ++curOrg;
            curOrgSize = 0;
            curQuality =
                randomQuality(numValidators - i >= minCritOrgSize
                                  ? ValidatorQuality::VALIDATOR_CRITICAL_QUALITY
                                  : ValidatorQuality::VALIDATOR_MED_QUALITY);
        }

        std::string const org = fmt::format("org-{}", curOrg);
        SecretKey const& key = sks.at(i) = SecretKey::random();

        ValidatorEntry& entry = validators.at(i);
        entry.mName = fmt::format("validator-{}", i);
        entry.mHomeDomain = org;
        entry.mQuality = curQuality;
        entry.mKey = key.getPublicKey();
        entry.mHasHistory = false;

        ++curOrgSize;
    }

    return {sks, validators};
}

// Expected weight of an org with quality `orgQuality` in a topology with a max
// quality of `maxQuality` and or quality counts of `orgQualityCounts`. This
// function normalizes the weight so that the highest quality has a weight of
// `1`.
static double
expectedOrgNormalizedWeight(
    std::unordered_map<ValidatorQuality, uint64> const& orgQualityCounts,
    ValidatorQuality maxQuality, ValidatorQuality orgQuality)
{
    if (orgQuality == ValidatorQuality::VALIDATOR_LOW_QUALITY)
    {
        return 0.0;
    }

    double normalizedWeight = 1.0;

    // For each quality level higher than `orgQuality`, divide the weight by 10
    // times the number of orgs at that quality level
    for (int q = static_cast<int>(maxQuality); q > static_cast<int>(orgQuality);
         --q)
    {
        normalizedWeight /=
            10 * orgQualityCounts.at(static_cast<ValidatorQuality>(q));
    }
    return normalizedWeight;
}

// Expected weight of a validator in an org of size `orgSize` with quality
// `orgQuality`.  `maxQuality` is the maximum quality present in the
// configuration. This function normalizes the weight so that the highest
// organization-level quality has a weight of `1`.
static double
expectedNormalizedWeight(
    std::unordered_map<ValidatorQuality, uint64> const& orgQualityCounts,
    ValidatorQuality maxQuality, ValidatorQuality orgQuality, int orgSize)
{
    return expectedOrgNormalizedWeight(orgQualityCounts, maxQuality,
                                       orgQuality) /
           orgSize;
}

// Collect information about the qualities and sizes of organizations in
// `validators` and store them in `maxQuality`, `orgQualities`, `orgSizes`, and
// `orgQualityCounts`.
static void
collectOrgInfo(ValidatorQuality& maxQuality,
               std::unordered_map<std::string, ValidatorQuality>& orgQualities,
               std::unordered_map<std::string, int>& orgSizes,
               std::unordered_map<ValidatorQuality, uint64>& orgQualityCounts,
               std::vector<ValidatorEntry> const& validators)
{
    maxQuality = ValidatorQuality::VALIDATOR_LOW_QUALITY;
    ValidatorQuality minQuality = ValidatorQuality::VALIDATOR_CRITICAL_QUALITY;
    std::unordered_map<ValidatorQuality, std::unordered_set<std::string>>
        orgsByQuality;
    for (ValidatorEntry const& validator : validators)
    {
        maxQuality = std::max(maxQuality, validator.mQuality);
        minQuality = std::min(minQuality, validator.mQuality);
        orgQualities[validator.mHomeDomain] = validator.mQuality;
        ++orgSizes[validator.mHomeDomain];
        orgsByQuality[validator.mQuality].insert(validator.mHomeDomain);
    }

    // Count orgs at each quality level
    for (int q = static_cast<int>(minQuality);
         q <= static_cast<int>(maxQuality); ++q)
    {
        orgQualityCounts[static_cast<ValidatorQuality>(q)] =
            orgsByQuality[static_cast<ValidatorQuality>(q)].size();
        if (q != static_cast<int>(minQuality))
        {
            // Add virtual org covering next lower quality level
            ++orgQualityCounts[static_cast<ValidatorQuality>(q)];
        }
    }
}

// Given a list of validators, test that the weights of the validators herder
// reports are correct
static void
testWeights(std::vector<ValidatorEntry> const& validators)
{
    Config cfg = getTestConfig(0);

    cfg.generateQuorumSetForTesting(validators);

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    // Collect info about orgs
    ValidatorQuality maxQuality;
    std::unordered_map<std::string, ValidatorQuality> orgQualities;
    std::unordered_map<std::string, int> orgSizes;
    std::unordered_map<ValidatorQuality, uint64> orgQualityCounts;
    collectOrgInfo(maxQuality, orgQualities, orgSizes, orgQualityCounts,
                   validators);

    // Check per-validator weights
    HerderImpl& herder = dynamic_cast<HerderImpl&>(app->getHerder());
    std::unordered_map<std::string, double> normalizedOrgWeights;
    for (ValidatorEntry const& validator : validators)
    {
        uint64_t weight = herder.getHerderSCPDriver().getNodeWeight(
            validator.mKey, cfg.QUORUM_SET, false);
        double normalizedWeight = static_cast<double>(weight) / UINT64_MAX;
        normalizedOrgWeights[validator.mHomeDomain] += normalizedWeight;

        std::string const& org = validator.mHomeDomain;
        REQUIRE_THAT(normalizedWeight,
                     Catch::Matchers::WithinAbs(
                         expectedNormalizedWeight(orgQualityCounts, maxQuality,
                                                  orgQualities.at(org),
                                                  orgSizes.at(org)),
                         0.0001));
    }

    // Check per-org weights
    for (auto const& [org, weight] : normalizedOrgWeights)
    {
        REQUIRE_THAT(
            weight, Catch::Matchers::WithinAbs(
                        expectedOrgNormalizedWeight(
                            orgQualityCounts, maxQuality, orgQualities.at(org)),
                        0.0001));
    }
}

// Test that HerderSCPDriver::getNodeWeight produces weights that result in a
// fair distribution of nomination wins.
TEST_CASE("getNodeWeight", "[herder]")
{
    SECTION("3 tier 1 validators, 1 org")
    {
        testWeights(simpleThreeNode().second);
    }

    SECTION("11 tier 1 validators, 3 unbalanced orgs")
    {
        testWeights(unbalancedOrgs().second);
    }

    SECTION("Tier1-like topology")
    {
        testWeights(teir1Like().second);
    }

    SECTION("Random topology")
    {
        // Test weights for 1000 random topologies of up to 200 validators
        for (int i = 0; i < 1000; ++i)
        {
            testWeights(randomTopology(200).second);
        }
    }
}

static Value
getRandomValue()
{
    auto h = sha256(fmt::format("value {}", gRandomEngine()));
    return xdr::xdr_to_opaque(h);
}

// A test version of NominationProtocol that exposes `updateRoundLeaders`
class TestNominationProtocol : public NominationProtocol
{
  public:
    TestNominationProtocol(Slot& slot) : NominationProtocol(slot)
    {
    }

    std::set<NodeID> const&
    updateRoundLeadersForTesting(
        std::optional<Value> const& previousValue = std::nullopt)
    {
        mPreviousValue = previousValue.value_or(getRandomValue());
        updateRoundLeaders();
        return getLeaders();
    }

    // Detect fast timeouts by examining the final round number
    bool
    fastTimedOut() const
    {
        return mRoundNumber > 0;
    }
};

// Test nomination over `numLedgers` slots. After running, check that the win
// percentages of each node and org are within 5% of the expected win
// percentages.
static void
testWinProbabilities(std::vector<SecretKey> const& sks,
                     std::vector<ValidatorEntry> const& validators,
                     int const numLedgers)
{
    REQUIRE(sks.size() == validators.size());

    // Collect info about orgs
    ValidatorQuality maxQuality;
    std::unordered_map<std::string, ValidatorQuality> orgQualities;
    std::unordered_map<std::string, int> orgSizes;
    std::unordered_map<ValidatorQuality, uint64> orgQualityCounts;
    collectOrgInfo(maxQuality, orgQualities, orgSizes, orgQualityCounts,
                   validators);

    // Generate a config
    Config cfg = getTestConfig();
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg.generateQuorumSetForTesting(validators);
    cfg.NODE_SEED = sks.front();

    // Create an application
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    // Run for `numLedgers` slots, recording the number of times each
    // node wins nomination
    UnorderedMap<NodeID, int> publishCounts;
    HerderImpl& herder = dynamic_cast<HerderImpl&>(app->getHerder());
    SCP& scp = herder.getSCP();
    int fastTimeouts = 0;
    for (int i = 0; i < numLedgers; ++i)
    {
        auto s = std::make_shared<Slot>(i, scp);
        TestNominationProtocol np(*s);

        std::set<NodeID> const& leaders = np.updateRoundLeadersForTesting();
        REQUIRE(leaders.size() == 1);
        for (NodeID const& leader : leaders)
        {
            ++publishCounts[leader];
        }

        if (np.fastTimedOut())
        {
            ++fastTimeouts;
        }
    }

    CLOG_INFO(Herder, "Fast Timeouts: {} ({}%)", fastTimeouts,
              fastTimeouts * 100.0 / numLedgers);

    // Compute total expected normalized weight across all nodes
    double totalNormalizedWeight = 0.0;
    for (ValidatorEntry const& validator : validators)
    {
        totalNormalizedWeight +=
            expectedNormalizedWeight(orgQualityCounts, maxQuality,
                                     orgQualities.at(validator.mHomeDomain),
                                     orgSizes.at(validator.mHomeDomain));
    }

    // Check validator win rates
    std::map<std::string, int> orgPublishCounts;
    for (ValidatorEntry const& validator : validators)
    {
        NodeID const& nodeID = validator.mKey;
        int publishCount = publishCounts[nodeID];

        // Compute and report node's win rate
        double winRate = static_cast<double>(publishCount) / numLedgers;
        CLOG_INFO(Herder, "Node {} win rate: {} (published {} ledgers)",
                  cfg.toShortString(nodeID), winRate, publishCount);

        // Expected win rate is `weight / total weight`
        double expectedWinRate =
            expectedNormalizedWeight(orgQualityCounts, maxQuality,
                                     orgQualities.at(validator.mHomeDomain),
                                     orgSizes.at(validator.mHomeDomain)) /
            totalNormalizedWeight;

        // Check that actual win rate is within .05 of expected win
        // rate.
        REQUIRE_THAT(winRate,
                     Catch::Matchers::WithinAbs(expectedWinRate, 0.05));

        // Record org publish counts for the next set of checks
        orgPublishCounts[validator.mHomeDomain] += publishCount;
    }

    // Check org win rates
    for (auto const& [org, count] : orgPublishCounts)
    {
        // Compute and report org's win rate
        double winRate = static_cast<double>(count) / numLedgers;
        CLOG_INFO(Herder, "Org {} win rate: {} (published {} ledgers)", org,
                  winRate, count);

        // Expected win rate is `weight / total weight`
        double expectedWinRate =
            expectedOrgNormalizedWeight(orgQualityCounts, maxQuality,
                                        orgQualities.at(org)) /
            totalNormalizedWeight;

        // Check that actual win rate is within .05 of expected win
        // rate.
        REQUIRE_THAT(winRate,
                     Catch::Matchers::WithinAbs(expectedWinRate, 0.05));
    }
}

// Test that the nomination algorithm produces a fair distribution of ledger
// publishers.
TEST_CASE("Fair nomination win rates", "[herder]")
{
    SECTION("3 tier 1 validators, 1 org")
    {
        auto [sks, validators] = simpleThreeNode();
        testWinProbabilities(sks, validators, 10000);
    }

    SECTION("11 tier 1 validators, 3 unbalanced orgs")
    {
        auto [sks, validators] = unbalancedOrgs();
        testWinProbabilities(sks, validators, 10000);
    }

    SECTION("Tier 1-like topology")
    {
        auto [sks, validators] = teir1Like();
        testWinProbabilities(sks, validators, 10000);
    }

    SECTION("Random topology")
    {
        for (int i = 0; i < 10; ++i)
        {
            auto [sks, validators] = randomTopology(50);
            testWinProbabilities(sks, validators, 10000);
        }
    }
}

// Returns a new `Topology` with the last org in `t` replaced with a new org
// with 3 validators. Requires that the last org in `t` have 3 validators and be
// contiguous at the back of the validators vecto.
static Topology
replaceOneOrg(Topology const& t)
{
    Topology t2(t); // Copy the topology
    auto& [sks, validators] = t2;
    REQUIRE(sks.size() == validators.size());

    // Give the org a unique name
    std::string const orgName = "org-replaced";

    // Double check that the new org name is unique
    for (ValidatorEntry const& v : validators)
    {
        REQUIRE(v.mHomeDomain != orgName);
    }

    // Remove the last org
    constexpr int validatorsPerOrg = 3;
    sks.resize(sks.size() - validatorsPerOrg);
    validators.resize(validators.size() - validatorsPerOrg);

    // Add new org with 3 validators
    int constexpr numValidators = 3;
    for (int j = 0; j < numValidators; ++j)
    {
        SecretKey const& key = sks.emplace_back(SecretKey::random());
        ValidatorEntry& entry = validators.emplace_back();
        entry.mName = fmt::format("validator-replaced-{}", j);
        entry.mHomeDomain = orgName;
        entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
        entry.mKey = key.getPublicKey();
        entry.mHasHistory = false;
    }

    return {sks, validators};
}

// Add `orgsToAdd` new orgs to the topology `t`. Each org will have 3
// validators.
static Topology
addOrgs(int orgsToAdd, Topology const& t)
{
    Topology t2(t); // Copy the topology
    auto& [sks, validators] = t2;
    REQUIRE(sks.size() == validators.size());

    // Generate new orgs
    for (int i = 0; i < orgsToAdd; ++i)
    {
        std::string const org = fmt::format("new-org-{}", i);
        int constexpr numValidators = 3;
        for (int j = 0; j < numValidators; ++j)
        {
            SecretKey const& key = sks.emplace_back(SecretKey::random());
            ValidatorEntry& entry = validators.emplace_back();
            entry.mName = fmt::format("new-validator-{}-{}", i, j);
            entry.mHomeDomain = org;
            entry.mQuality = ValidatorQuality::VALIDATOR_HIGH_QUALITY;
            entry.mKey = key.getPublicKey();
            entry.mHasHistory = false;
        }
    }
    return t2;
}

// Returns `true` if the set intersection of `leaders1` and `leaders2` is not
// empty.
bool
leadersIntersect(std::set<NodeID> const& leaders1,
                 std::set<NodeID> const& leaders2)
{
    std::vector<NodeID> intersection;
    std::set_intersection(leaders1.begin(), leaders1.end(), leaders2.begin(),
                          leaders2.end(), std::back_inserter(intersection));
    return !intersection.empty();
}

// Given two quorum sets consisting of validators in `validators1` and
// `validators2`, this function returns the probability that the two quorum sets
// will agree on a leader in the first round of nomination.
double
computeExpectedFirstRoundAgreementProbability(
    std::vector<ValidatorEntry> const& validators1,
    std::vector<ValidatorEntry> const& validators2)
{
    // Gather orgs
    std::set<std::string> orgs1;
    std::transform(validators1.begin(), validators1.end(),
                   std::inserter(orgs1, orgs1.end()),
                   [](ValidatorEntry const& v) { return v.mHomeDomain; });
    std::set<std::string> orgs2;
    std::transform(validators2.begin(), validators2.end(),
                   std::inserter(orgs2, orgs2.end()),
                   [](ValidatorEntry const& v) { return v.mHomeDomain; });

    // Compute overlap
    std::vector<std::string> sharedOrgs;
    std::set_intersection(orgs1.begin(), orgs1.end(), orgs2.begin(),
                          orgs2.end(), std::back_inserter(sharedOrgs));

    // Probability of agreement in first round is (orgs overlapping / orgs1) *
    // (orgs overlapping / orgs2). That's the probability that the two sides
    // will pick any overlapping org. The algorithm guarantees that if they pick
    // overlapping validator, they'll pick the same validator.
    double overlap = static_cast<double>(sharedOrgs.size());
    return overlap / orgs1.size() * overlap / orgs2.size();
}

// Test that the nomination algorithm behaves as expected when the two quorum
// sets `qs1` and `qs2` are not equivalent. This function requires that both
// quorum sets overlap, and contain only a single quality level of validators.
// Runs simulation for `numLedgers` slots.
// NOTE: This test counts any failure to agree on a leader as a timeout. In
// practice, it's possible that one side of the split is large enough to proceed
// without the other side. In this case, the larger side might not experience a
// timeout and "drag" the other side through consensus with it. However, this
// test aims to analyze the worst case scenario where the two sides are fairly
// balanced and real-world networking conditions are in place (some nodes
// lagging, etc), such that disagreement always results in a timeout.
void
testAsymmetricTimeouts(Topology const& qs1, Topology const& qs2,
                       int const numLedgers)
{
    auto const& [sks1, validators1] = qs1;
    auto const& [sks2, validators2] = qs2;

    REQUIRE(sks1.size() == validators1.size());
    REQUIRE(sks2.size() == validators2.size());

    // Generate configs and nodes representing one validator with each quorum
    // set
    std::vector<VirtualClock> clocks(2);
    std::vector<Application::pointer> apps;
    for (int i = 0; i < 2; ++i)
    {
        Config cfg = getTestConfig(i);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.generateQuorumSetForTesting(i == 0 ? validators1 : validators2);
        cfg.NODE_SEED = i == 0 ? sks1.back() : sks2.back();

        auto app = apps.emplace_back(createTestApplication(clocks.at(i), cfg));
    }

    // Run the nomination algorithm for `numLedgers` slots. Simulate timeouts by
    // re-running slots that don't agree on a leader until their leader
    // elections overlap. Record the number of timeouts it takes for the two
    // quorum sets to agree on a leader in `timeouts`, which is effectively a
    // mapping from number of timeouts to the number of ledgers that experienced
    // that many timeouts.
    std::vector<int> timeouts(std::max(validators1.size(), validators2.size()));
    for (int i = 0; i < numLedgers; ++i)
    {
        Value const v = getRandomValue();
        SCP& scp1 = dynamic_cast<HerderImpl&>(apps.at(0)->getHerder()).getSCP();
        SCP& scp2 = dynamic_cast<HerderImpl&>(apps.at(1)->getHerder()).getSCP();
        auto s1 = std::make_shared<Slot>(i, scp1);
        auto s2 = std::make_shared<Slot>(i, scp2);

        TestNominationProtocol np1(*s1);
        TestNominationProtocol np2(*s2);

        for (int j = 0; j < timeouts.size(); ++j)
        {
            std::set<NodeID> const& leaders1 =
                np1.updateRoundLeadersForTesting(v);
            std::set<NodeID> const& leaders2 =
                np2.updateRoundLeadersForTesting(v);
            REQUIRE(leaders1.size() == j + 1);
            REQUIRE(leaders2.size() == j + 1);

            if (leadersIntersect(leaders1, leaders2))
            {
                // Agreed on a leader! Record the number of timeouts resulted.
                ++timeouts.at(j);
                break;
            }
        }

        // If leaders don't intersect after running through the loop then the
        // two quorum sets have no overlap and the test is broken.
        REQUIRE(leadersIntersect(np1.getLeaders(), np2.getLeaders()));
    }

    // For the first round, we can easily compute the expected agreement
    // probability. For subsequent rounds, we check only that the success rate
    // increases over time (modulo some small epsilon).
    double expectedSuccessRate =
        computeExpectedFirstRoundAgreementProbability(validators1, validators2);

    // Allow for some small decrease in success rate from the theoretical value.
    // We're working with probabilistic simulation here so we can't be too
    // strict or the test will be flaky.
    double constexpr epsilon = 0.1;

    // There's not enough data in the tail of the distribution to allow us to
    // assert that the success rate is what's expected. To avoid sporadic test
    // failures, we cut off `tailCutoffPoint` of the tail of the distribution
    // for the purposes of asserting test values. However, the test will still
    // log those success rates for manual examination.
    double constexpr tailCutoffPoint = 0.05;

    int numLedgersRemaining = numLedgers;
    for (int i = 0; i < timeouts.size(); ++i)
    {
        int const numTimeouts = timeouts.at(i);
        if (numTimeouts == 0)
        {
            // Avoid cluttering output
            continue;
        }

        CLOG_INFO(Herder, "Ledgers with {} timeouts: {} ({}%)", i, numTimeouts,
                  static_cast<double>(numTimeouts) * 100 / numLedgers);

        if (numLedgersRemaining > numLedgers * tailCutoffPoint)
        {
            // Check that success rate increases over time. Allow some epsilon
            // decrease because this is a probabilistic simulation. Also stop
            // checking when we're at the last `tailCutoffPoint` timeouts as the
            // data is too sparse to be useful.
            double successRate =
                static_cast<double>(timeouts.at(i)) / numLedgersRemaining;
            REQUIRE(successRate > expectedSuccessRate - epsilon);

            // Take max of success rate and previous success rate to avoid
            // accidentally accepting a declining success rate due to episilon.
            expectedSuccessRate = std::max(successRate, expectedSuccessRate);
            numLedgersRemaining -= numTimeouts;
        }
    }
}

// Test timeouts with asymmetric quorums. This test serves two purposes:
// 1. It contains assertions checking for moderate (10%) deviations from the
//    expected behavior of the nomination algorithm. These should detect any
//    major issues/regressions with the algorithm.
// 2. It logs the distributions of timeouts for manual inspection. This is
//    useful for understanding the behavior of the algorithm and for testing
//    specific scenarios one might be interested in (e.g., if tier 1 disagrees
//    on one org's presence in tier 1, what is the impact on nomination
//    timeouts?).
// NOTE: This provides a worst-case analysis of timeouts. See the NOTE on
// `testAsymmetricTimeouts` for more details.
TEST_CASE("Asymmetric quorum timeouts", "[herder]")
{
    // Number of slots to run for
    int constexpr numLedgers = 20000;

    SECTION("Tier 1-like topology with replaced org")
    {
        auto t = teir1Like();
        testAsymmetricTimeouts(t, replaceOneOrg(t), numLedgers);
    }

    SECTION("Tier 1-like topology with 1 added org")
    {
        auto t = teir1Like();
        testAsymmetricTimeouts(t, addOrgs(1, t), numLedgers);
    }

    SECTION("Tier 1-like topology with 3 added orgs")
    {
        auto t = teir1Like();
        testAsymmetricTimeouts(t, addOrgs(3, t), numLedgers);
    }
}

// Test that the nomination algorithm behaves as expected when a random
// `numUnresponsive` set of nodes in `qs` are unresponsive.  Runs simulation for
// `numLedgers` slots.
void
testUnresponsiveTimeouts(Topology const& qs, int numUnresponsive,
                         int const numLedgers)
{
    auto const& [sks, validators] = qs;
    REQUIRE(sks.size() == validators.size());
    REQUIRE(numUnresponsive < validators.size());

    // extract and shuffle node ids. Choose `numUnresponsive` nodes to be the
    // unresponsive nodes.
    std::vector<NodeID> nodeIDs;
    std::transform(validators.begin(), validators.end(),
                   std::back_inserter(nodeIDs),
                   [](ValidatorEntry const& v) { return v.mKey; });
    stellar::shuffle(nodeIDs.begin(), nodeIDs.end(), gRandomEngine);
    std::set<NodeID> unresponsive(nodeIDs.begin(),
                                  nodeIDs.begin() + numUnresponsive);

    // Collect info about orgs
    ValidatorQuality maxQuality;
    std::unordered_map<std::string, ValidatorQuality> orgQualities;
    std::unordered_map<std::string, int> orgSizes;
    std::unordered_map<ValidatorQuality, uint64> orgQualityCounts;
    collectOrgInfo(maxQuality, orgQualities, orgSizes, orgQualityCounts,
                   validators);

    // Compute total weight of all validators, as well as the total weight of
    // unresponsive validators
    double totalWeight = 0.0;
    double unresponsiveWeight = 0.0;
    for (ValidatorEntry const& validator : validators)
    {
        double normalizedWeight =
            expectedNormalizedWeight(orgQualityCounts, maxQuality,
                                     orgQualities.at(validator.mHomeDomain),
                                     orgSizes.at(validator.mHomeDomain));
        totalWeight += normalizedWeight;
        if (unresponsive.count(validator.mKey))
        {
            unresponsiveWeight += normalizedWeight;
        }
    }

    // Compute the average weight of an unresponsive node
    double avgUnresponsiveWeight = unresponsiveWeight / numUnresponsive;

    // Compute expected number of ledgers experiencing `n` timeouts where `n` is
    // the index of the `timeouts` vector. This vector is a mapping from number
    // of timeouts to expected number of ledgers experiencing that number of
    // timeouts.
    std::vector<int> expectedTimeouts(numUnresponsive + 1);
    double remainingWeight = totalWeight;
    int remainingUnresponsive = numUnresponsive;
    int remainingLedgers = numLedgers;
    for (int i = 0; i < expectedTimeouts.size(); ++i)
    {
        double timeoutProb =
            (avgUnresponsiveWeight * remainingUnresponsive) / remainingWeight;
        // To get expected number of ledgers experiencing `i` timeouts, we take
        // the probability a timeout does not occur and multiply it by the
        // number of remaining ledgers.
        int expectedLedgers = (1 - timeoutProb) * remainingLedgers;
        expectedTimeouts.at(i) = expectedLedgers;

        // Remaining ledgers decreases by expected number of ledgers
        // experiencing `i` timeouts
        remainingLedgers -= expectedLedgers;

        // For `i+1` timeouts to occur, an unresponsive node must be chosen.
        // Therefore, deduct the average weight of an unresponsive node from the
        // total weight left in the network.
        remainingWeight -= avgUnresponsiveWeight;
        --remainingUnresponsive;
    }

    // Generate a config
    Config cfg = getTestConfig();
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg.generateQuorumSetForTesting(validators);
    cfg.NODE_SEED = sks.front();

    // Create an application
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    // Run for `numLedgers` slots, recording the number of times each slot timed
    // out due to unresponsive nodes before successfully electing a responsive
    // leader.
    SCP& scp = dynamic_cast<HerderImpl&>(app->getHerder()).getSCP();
    std::vector<int> timeouts(numUnresponsive + 1);
    for (int i = 0; i < numLedgers; ++i)
    {
        Value const v = getRandomValue();
        auto s = std::make_shared<Slot>(i, scp);

        TestNominationProtocol np(*s);
        for (int i = 0; i < timeouts.size(); ++i)
        {
            std::set<NodeID> const& leaders =
                np.updateRoundLeadersForTesting(v);
            // If leaders is a subset of unresponsive, then a timeout occurs.
            if (!std::includes(unresponsive.begin(), unresponsive.end(),
                               leaders.begin(), leaders.end()))
            {
                ++timeouts.at(i);
                break;
            }
        }
    }

    // Allow for some small multiplicative increase in timeouts from the
    // theoretical value.  We're working with probabilistic simulation here so
    // we can't be too strict or the test will be flaky.
    double constexpr epsilon = 1.1;

    // There's not enough data in the tail of the distribution to allow us to
    // assert that the timeout values are what's expected. To avoid sporadic
    // test failures, we cut off `tailCutoffPoint` of the tail of the
    // distribution for the purposes of asserting test values. However, the test
    // will still log those values for manual examination.
    double constexpr tailCutoffPoint = 0.05;

    // Analyze timeouts
    int numLedgersRemaining = numLedgers;
    for (int i = 0; i < timeouts.size(); ++i)
    {
        int const numTimeouts = timeouts.at(i);
        int const expectedNumTimeouts = expectedTimeouts.at(i);

        if (numLedgersRemaining > numLedgers * tailCutoffPoint)
        {
            // Check that timeouts are less than epsilon times the expected
            // value. Also stop checking when we're at the last
            // `tailCutoffPoint` timeouts as the data is too sparse to be
            // useful.
            REQUIRE(numTimeouts < expectedNumTimeouts * epsilon);
        }
        CLOG_INFO(Herder, "Ledgers with {} timeouts: {} ({}%)", i, numTimeouts,
                  numTimeouts * 100.0 / numLedgers);
        numLedgersRemaining -= numTimeouts;
    }
}

// Test timeouts for a tier 1-like topology with 1-5 unresponsive nodes. This
// test serves two purposes:
// 1. It contains assertions checking for moderate (10%) deviations from the
//    expected behavior of the nomination algorithm. These should detect any
//    major issues/regressions with the algorithm.
// 2. It logs the distributions of timeouts for manual inspection. This is
//    useful for understanding the behavior of the algorithm and for testing
//    specific scenarios one might be interested in (e.g., if 3 tier 1 nodes
//    are heavily lagging, what is the impact on nomination timeouts?).
TEST_CASE("Unresponsive quorum timeouts", "[herder]")
{
    // Number of slots to run for
    int constexpr numLedgers = 20000;

    auto t = teir1Like();
    for (int i = 1; i <= 5; ++i)
    {
        CLOG_INFO(Herder, "Simulating nomination with {} unresponsive nodes",
                  i);
        testUnresponsiveTimeouts(t, i, numLedgers);
    }
}