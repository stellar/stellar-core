// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "database/Database.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "ledger/TrustLineReference.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

// *XLM Payment
// *Credit Payment
// XLM -> Credit Payment
// Credit -> XLM Payment
// Credit -> XLM -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx][payment]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);

    Asset xlm;

    int64_t txfee = getCurrentTxFee(app->getLedgerStateRoot());

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        getCurrentMinBalance(app->getLedgerStateRoot(), 2) + 10 * txfee;

    // minimum balance necessary to hold 2 trust lines and an offer
    const int64_t minBalance3 =
        getCurrentMinBalance(app->getLedgerStateRoot(), 3) + 10 * txfee;

    const int64_t paymentAmount = minBalance2;

    // create an account
    auto a1 = root.create("A", paymentAmount);

    const int64_t morePayment = paymentAmount / 2;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000;

    // sets up gateway account
    const int64_t gatewayPayment = minBalance2 + morePayment;
    auto gateway = root.create("gate", gatewayPayment);

    // sets up gateway2 account
    auto gateway2 = root.create("gate2", gatewayPayment);

    Asset idr = makeAsset(gateway, "IDR");
    Asset usd = makeAsset(gateway2, "USD");

    int64_t rootBalance = 0;
    int64_t a1Balance = 0;
    {
        LedgerState ls(app->getLedgerStateRoot());
        auto rootAccount = stellar::loadAccount(ls, root.getPublicKey());
        REQUIRE(rootAccount);
        auto a1Account = stellar::loadAccount(ls, a1.getPublicKey());
        REQUIRE(a1Account);
        REQUIRE(rootAccount.getMasterWeight() == 1);
        REQUIRE(rootAccount.getHighThreshold() == 0);
        REQUIRE(rootAccount.getLowThreshold() == 0);
        REQUIRE(rootAccount.getMediumThreshold() == 0);
        REQUIRE(a1Account.getBalance() == paymentAmount);
        REQUIRE(a1Account.getMasterWeight() == 1);
        REQUIRE(a1Account.getHighThreshold() == 0);
        REQUIRE(a1Account.getLowThreshold() == 0);
        REQUIRE(a1Account.getMediumThreshold() == 0);
        // root did 2 transactions at this point
        REQUIRE(rootAccount.getBalance() == (1000000000000000000 - paymentAmount -
                                              gatewayPayment * 2 - txfee * 3));
        rootBalance = rootAccount.getBalance();
        a1Balance = a1Account.getBalance();
    }

    closeLedgerOn(*app, 2, 1, 1, 2016);

    SECTION("Create account")
    {
        SECTION("Success")
        {
            for_all_versions(*app, [&] {
                auto b1 =
                    root.create("B", getCurrentMinBalance(app->getLedgerStateRoot(), 0));
                SECTION("Account already exists")
                {
                    REQUIRE_THROWS_AS(
                        root.create("B",
                                    getCurrentMinBalance(app->getLedgerStateRoot(), 0)),
                        ex_CREATE_ACCOUNT_ALREADY_EXIST);
                }
            });
        }
        SECTION("Not enough funds (source)")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(gateway.create("B", gatewayPayment),
                                  ex_CREATE_ACCOUNT_UNDERFUNDED);
            });
        }
        SECTION("Amount too small to create account")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    root.create("B",
                                getCurrentMinBalance(app->getLedgerStateRoot(), 0) - 1),
                    ex_CREATE_ACCOUNT_LOW_RESERVE);
            });
        }
    }

    SECTION("a pays b, then a merge into b")
    {
        auto paymentAmountMerge = 1000000;
        auto amount =
            getCurrentMinBalance(app->getLedgerStateRoot(), 0) + paymentAmountMerge;
        auto b1 = root.create("B", amount);

        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();
        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto txFrame = a1.tx({payment(b1, 200), accountMerge(b1)});

        for_all_versions(*app, [&] {
            applyCheck(txFrame, *app);

            REQUIRE(!hasAccount(*app, a1));
            REQUIRE(hasAccount(*app, b1));
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            REQUIRE(!hasAccount(*app, a1));
            REQUIRE((a1Balance + b1Balance - txFrame->getFee()) ==
                    b1.getBalance());
        });
    }

    SECTION("a pays b, then b merge into a")
    {
        auto paymentAmountMerge = 1000000;
        auto amount =
            getCurrentMinBalance(app->getLedgerStateRoot(), 0) + paymentAmountMerge;
        auto b1 = root.create("B", amount);

        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto txFrame = a1.tx({payment(b1, 200), b1.op(accountMerge(a1))});
        txFrame->addSignature(b1);

        for_all_versions(*app, [&] {
            applyCheck(txFrame, *app);

            REQUIRE(hasAccount(*app, a1));
            REQUIRE(!hasAccount(*app, b1));
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            REQUIRE(!hasAccount(*app, b1));
            REQUIRE((a1Balance + b1Balance - txFrame->getFee()) ==
                    a1.getBalance());
        });
    }

    SECTION("merge then send")
    {
        auto b1 = root.create("B", getCurrentMinBalance(app->getLedgerStateRoot(), 0));

        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto txFrame = a1.tx({accountMerge(b1), payment(b1, 200)});

        for_versions_to(7, *app, [&] {
            applyCheck(txFrame, *app);

            REQUIRE(hasAccount(*app, a1));
            REQUIRE(hasAccount(*app, b1));
            REQUIRE(txFrame->getResultCode() == txINTERNAL_ERROR);

            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFee()) == a1.getBalance());
        });

        for_versions_from(8, *app, [&] {
            applyCheck(txFrame, *app);

            REQUIRE(hasAccount(*app, a1));
            REQUIRE(hasAccount(*app, b1));
            REQUIRE(txFrame->getResultCode() == txFAILED);

            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFee()) == a1.getBalance());
        });
    }

    SECTION("send XLM to an existing account")
    {
        for_all_versions(*app, [&] {
            root.pay(a1, morePayment);

            LedgerState ls(app->getLedgerStateRoot());
            auto rootAccount = stellar::loadAccount(ls, root.getPublicKey());
            REQUIRE(rootAccount);
            auto a1Account = stellar::loadAccount(ls, a1.getPublicKey());
            REQUIRE(a1Account);
            REQUIRE(a1Account.getBalance() == a1Balance + morePayment);
            // root did 2 transactions at this point
            REQUIRE(rootAccount.getBalance() ==
                    (rootBalance - morePayment - txfee));
        });
    }

    SECTION("send XLM to a new account (no destination)")
    {
        for_all_versions(*app, [&] {
            LedgerState ls1(app->getLedgerStateRoot());
            auto baseReserve = ls1.loadHeader()->header().baseReserve;
            ls1.rollback();

            REQUIRE_THROWS_AS(root.pay(getAccount("B").getPublicKey(),
                                       2* baseReserve),
                              ex_PAYMENT_NO_DESTINATION);

            LedgerState ls2(app->getLedgerStateRoot());
            auto rootAccount = stellar::loadAccount(ls2, root.getPublicKey());
            REQUIRE(rootAccount);
            REQUIRE(rootAccount.getBalance() == (rootBalance - txfee));
        });
    }

    SECTION("rescue account (was below reserve)")
    {
        for_all_versions(*app, [&] {
            int64 orgReserve = getCurrentMinBalance(app->getLedgerStateRoot(), 0);

            auto b1 = root.create("B", orgReserve + 1000);

            // raise the reserve
            uint32 addReserve = 100000;
            LedgerState ls(app->getLedgerStateRoot());
            ls.loadHeader()->header().baseReserve += addReserve;
            ls.commit();

            // verify that the account can't do anything
            auto tx = b1.tx({payment(root, 1)});
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(tx->getResultCode() == txINSUFFICIENT_BALANCE);

            // top up the account to unblock it
            int64 topUp = getCurrentMinBalance(app->getLedgerStateRoot(), 0) - orgReserve;
            root.pay(b1, topUp);

            // payment goes through
            b1.pay(root, 1);
        });
    }

    SECTION("two payments, first breaking second")
    {
        int64 startingBalance = paymentAmount + 5 +
                                getCurrentMinBalance(app->getLedgerStateRoot(), 0) +
                                txfee * 2;
        auto b1 = root.create("B", startingBalance);
        auto tx1 = b1.tx({payment(root, paymentAmount)});
        auto tx2 = b1.tx({payment(root, 6)});
        auto rootBalance = root.getBalance();

        for_versions_to(8, *app, [&] {
            auto r = closeLedgerOn(*app, 3, 1, 2, 2016, {tx1, tx2});
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txINSUFFICIENT_BALANCE);

            int64 expectedrootBalance = rootBalance + paymentAmount;
            int64 expectedb1Balance =
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + 5;
            REQUIRE(expectedb1Balance == b1.getBalance());
            REQUIRE(expectedrootBalance == root.getBalance());
        });

        for_versions_from(9, *app, [&] {
            auto r = closeLedgerOn(*app, 3, 1, 2, 2016, {tx1, tx2});
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txFAILED);
            REQUIRE(r[1].first.result.result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_UNDERFUNDED);

            int64 expectedrootBalance = rootBalance + paymentAmount;
            int64 expectedb1Balance =
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + 5;
            REQUIRE(expectedb1Balance == b1.getBalance());
            REQUIRE(expectedrootBalance == root.getBalance());
        });
    }

    SECTION("create, merge, pay, 2 accounts")
    {
        auto amount = 300000000000000;
        auto createAmount = 500000000;
        auto payAmount = 200000000;
        auto sourceAccount = root.create("source", amount);
        auto createSourceAccount = TestAccount{*app, getAccount("create")};
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx =
            sourceAccount.tx({createAccount(createSourceAccount, createAmount),
                              accountMerge(createSourceAccount),
                              payment(sourceAccount, payAmount)});

        for_versions_to(7, *app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(!hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, createSourceAccount));
            REQUIRE(createSourceAccount.getBalance() == amount - tx->getFee());

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - createAmount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(!hasAccount(*app, createSourceAccount));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - createAmount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
        });
    }

    SECTION("create, merge, pay, 3 accounts")
    {
        auto amount = 300000000000000;
        auto createAmount = 500000000;
        auto payAmount = 200000000;
        auto sourceAccount = root.create("source", amount);
        auto createSourceAccount = TestAccount{*app, getAccount("create")};
        auto payAccount = root.create("pay", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();

        auto tx =
            sourceAccount.tx({createAccount(createSourceAccount, createAmount),
                              accountMerge(createSourceAccount),
                              payment(payAccount, payAmount)});

        for_versions_to(7, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(!hasAccount(*app, createSourceAccount));
            REQUIRE(hasAccount(*app, payAccount));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(payAccount.getBalance() == amount);

            REQUIRE(tx->getResult().result.code() == txINTERNAL_ERROR);
        });

        for_versions(8, 9, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(!hasAccount(*app, createSourceAccount));
            REQUIRE(hasAccount(*app, payAccount));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(payAccount.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - createAmount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
        });
        // 10 and above, operation #2 fails (already covered in merge tests)
    }

    SECTION("pay, merge, create, pay, self")
    {
        auto amount = 300000000000000;
        auto pay1Amount = 100000000;
        auto createAmount = 500000000;
        auto pay2Amount = 200000000;
        auto sourceAccount = root.create("source", amount);
        auto payAndMergeDestination = root.create("payAndMerge", amount);
        auto payAndMergeDestinationSeqNum =
            payAndMergeDestination.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx =
            sourceAccount.tx({payAndMergeDestination.op(
                                  payment(payAndMergeDestination, pay1Amount)),
                              accountMerge(payAndMergeDestination),
                              payAndMergeDestination.op(
                                  createAccount(sourceAccount, createAmount)),
                              payAndMergeDestination.op(payment(
                                  payAndMergeDestination, pay2Amount))});
        tx->addSignature(payAndMergeDestination);

        for_versions_to(7, *app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, payAndMergeDestination));
            REQUIRE(sourceAccount.getBalance() == createAmount);
            REQUIRE(payAndMergeDestination.getBalance() ==
                    amount + amount - createAmount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == 0x400000000ull);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() ==
                    payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, *app, [&] {
            // as the account gets re-created we have to disable seqnum
            // verification
            REQUIRE(applyCheck(tx, *app, false));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, payAndMergeDestination));
            REQUIRE(sourceAccount.getBalance() == createAmount);
            REQUIRE(payAndMergeDestination.getBalance() ==
                    amount + amount - createAmount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == 0x400000000ull);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() ==
                    payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });
    }

    SECTION("pay, merge, create, pay, 2 accounts")
    {
        auto amount = 300000000000000;
        auto pay1Amount = 500000000;
        auto createAmount = 250000000;
        auto pay2Amount = 10000000;
        auto sourceAccount = root.create("source", amount);
        auto payAndMergeDestination = root.create("payAndMerge", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto payAndMergeDestinationSeqNum =
            payAndMergeDestination.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx =
            sourceAccount.tx({payment(payAndMergeDestination, pay1Amount),
                              accountMerge(payAndMergeDestination),
                              payAndMergeDestination.op(
                                  createAccount(sourceAccount, createAmount)),
                              payment(payAndMergeDestination, pay2Amount)});
        tx->addSignature(payAndMergeDestination);

        for_versions_to(7, *app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, payAndMergeDestination));
            REQUIRE(sourceAccount.getBalance() ==
                    amount - pay1Amount - pay2Amount - tx->getFee());
            REQUIRE(payAndMergeDestination.getBalance() ==
                    amount + amount + pay2Amount - tx->getFee() - createAmount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() ==
                    payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, *app, [&] {
            // as the account gets re-created we have to disable seqnum
            // verification
            REQUIRE(applyCheck(tx, *app, false));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, payAndMergeDestination));
            REQUIRE(sourceAccount.getBalance() == createAmount - pay2Amount);
            REQUIRE(payAndMergeDestination.getBalance() ==
                    amount + amount + pay2Amount - tx->getFee() - createAmount);
            REQUIRE(sourceAccount.loadSequenceNumber() == 0x400000000ull);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() ==
                    payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });
    }

    SECTION("pay, merge, create, pay, 3 accounts")
    {
        auto amount = 300000000000000;
        auto pay1Amount = 500000000;
        auto createAmount = 250000000;
        auto pay2Amount = 10000000;
        auto sourceAccount = root.create("source", amount);
        auto secondSourceAccount = root.create("secondSource", amount);
        auto payAndMergeDestination = root.create("payAndMerge", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto secondSourceSeqNum = secondSourceAccount.getLastSequenceNumber();
        auto payAndMergeDestinationSeqNum =
            payAndMergeDestination.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx = sourceAccount.tx(
            {payment(payAndMergeDestination, pay1Amount),
             accountMerge(payAndMergeDestination),
             secondSourceAccount.op(createAccount(sourceAccount, createAmount)),
             payment(payAndMergeDestination, pay2Amount)});
        tx->addSignature(secondSourceAccount);

        for_versions_to(7, *app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, secondSourceAccount));
            REQUIRE(hasAccount(*app, payAndMergeDestination));
            REQUIRE(sourceAccount.getBalance() ==
                    amount - pay1Amount - pay2Amount - tx->getFee());
            REQUIRE(secondSourceAccount.getBalance() == amount - createAmount);
            REQUIRE(payAndMergeDestination.getBalance() ==
                    amount + amount + pay2Amount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(secondSourceAccount.loadSequenceNumber() ==
                    secondSourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() ==
                    payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, *app, [&] {
            // as the account gets re-created we have to disable seqnum
            // verification
            REQUIRE(applyCheck(tx, *app, false));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, secondSourceAccount));
            REQUIRE(hasAccount(*app, payAndMergeDestination));
            REQUIRE(sourceAccount.getBalance() == createAmount - pay2Amount);
            REQUIRE(secondSourceAccount.getBalance() == amount - createAmount);
            REQUIRE(payAndMergeDestination.getBalance() ==
                    amount + amount + pay2Amount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == 0x400000000ull);
            REQUIRE(secondSourceAccount.loadSequenceNumber() ==
                    secondSourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() ==
                    payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() ==
                    amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
        });
    }

    SECTION("create, path payment, merge, create")
    {
        auto amount = 300000000000000;
        auto create1Amount = 1000000000;
        auto payAmount = 50000000;
        auto create2Amount = 2000000000;
        auto sourceAccount = root.create("source", amount);
        auto createSource = root.create("createSource", amount);
        auto createDestination =
            TestAccount{*app, getAccount("createDestination")};
        auto payDestination = root.create("pay", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto createSourceSeqNum = createSource.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx = sourceAccount.tx({
            createSource.op(createAccount(createDestination, create1Amount)),
            createDestination.op(pathPayment(payDestination, xlm, payAmount,
                                             xlm, payAmount, {})),
            payDestination.op(accountMerge(createSource)),
            createSource.op(createAccount(payDestination, create2Amount)),
        });
        tx->addSignature(createSource);
        tx->addSignature(createDestination);
        tx->addSignature(payDestination);

        for_all_versions(*app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, createSource));
            REQUIRE(hasAccount(*app, createDestination));
            REQUIRE(hasAccount(*app, payDestination));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(createSource.getBalance() == amount + amount -
                                                     create1Amount -
                                                     create2Amount + payAmount);
            REQUIRE(createDestination.getBalance() ==
                    create1Amount - payAmount);
            REQUIRE(payDestination.getBalance() == create2Amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(createSource.loadSequenceNumber() == createSourceSeqNum);
            REQUIRE(createDestination.loadSequenceNumber() == 0x400000000ull);
            REQUIRE(payDestination.loadSequenceNumber() == 0x400000000ull);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() ==
                    PATH_PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .pathPaymentResult()
                        .code() == PATH_PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() ==
                    ACCOUNT_MERGE);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount + payAmount);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() ==
                    CREATE_ACCOUNT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .createAccountResult()
                        .code() == CREATE_ACCOUNT_SUCCESS);
        });
    }

    SECTION("pay self, merge, pay self, merge")
    {
        auto amount = 300000000000000;
        auto pay1Amount = 500000000;
        auto pay2Amount = 250000000;
        auto sourceAccount = root.create("source", amount);
        auto mergeDestination = root.create("payAndMerge", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto mergeDestinationSeqNum = mergeDestination.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx = sourceAccount.tx({payment(sourceAccount, pay1Amount),
                                    accountMerge(mergeDestination),
                                    payment(sourceAccount, pay2Amount),
                                    accountMerge(mergeDestination)});

        for_versions_to(4, *app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(!hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, mergeDestination));
            REQUIRE(mergeDestination.getBalance() ==
                    amount + amount + amount - tx->getFee() - tx->getFee());
            REQUIRE(mergeDestination.loadSequenceNumber() ==
                    mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
        });

        for_versions(5, 7, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, mergeDestination));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() ==
                    mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_NO_ACCOUNT);
        });

        for_versions_from(8, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, mergeDestination));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() ==
                    mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[3].code() == opNO_ACCOUNT);
        });
    }

    SECTION("pay self multiple, merge, pay self multiple, merge")
    {
        auto amount = 300000000000000;
        auto pay1Amount = 500000000;
        auto pay2Amount = 250000000;
        auto sourceAccount = root.create("source", amount);
        auto mergeDestination = root.create("payAndMerge", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto mergeDestinationSeqNum = mergeDestination.getLastSequenceNumber();

        closeLedgerOn(*app, 3, 1, 2, 2016);

        auto tx = sourceAccount.tx({payment(sourceAccount, pay1Amount),
                                    payment(sourceAccount, pay1Amount),
                                    payment(sourceAccount, pay1Amount),
                                    payment(sourceAccount, pay1Amount),
                                    payment(sourceAccount, pay1Amount),
                                    payment(sourceAccount, pay1Amount),
                                    accountMerge(mergeDestination),
                                    payment(sourceAccount, pay2Amount),
                                    payment(sourceAccount, pay2Amount),
                                    accountMerge(mergeDestination)});

        for_versions_to(4, *app, [&] {
            REQUIRE(applyCheck(tx, *app));
            REQUIRE(!hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, mergeDestination));
            REQUIRE(mergeDestination.getBalance() ==
                    amount + amount + amount - tx->getFee() - tx->getFee());
            REQUIRE(mergeDestination.loadSequenceNumber() ==
                    mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[4].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[4].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[4]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[5].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[5].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[5]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[6]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[6]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[7].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[7].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[7]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[8].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[8].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[8]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[9].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[9]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[9]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
        });

        for_versions(5, 7, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, mergeDestination));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() ==
                    mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[4].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[4].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[4]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[5].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[5].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[5]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[6]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[6]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[7].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[7].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[7]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[8].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[8].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[8]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[9].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[9]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_NO_ACCOUNT);
        });

        for_versions_from(8, *app, [&] {
            REQUIRE(!applyCheck(tx, *app));
            REQUIRE(hasAccount(*app, sourceAccount));
            REQUIRE(hasAccount(*app, mergeDestination));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() ==
                    mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[0]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[1]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[2]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[3]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[4].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[4].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[4]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[5].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[5].tr().type() == PAYMENT);
            REQUIRE(tx->getResult()
                        .result.results()[5]
                        .tr()
                        .paymentResult()
                        .code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].code() == opINNER);
            REQUIRE(tx->getResult()
                        .result.results()[6]
                        .tr()
                        .accountMergeResult()
                        .code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult()
                        .result.results()[6]
                        .tr()
                        .accountMergeResult()
                        .sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[7].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[8].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[9].code() == opNO_ACCOUNT);
        });
    }

    auto loadTrustLineAndCheckBalance =
        [&] (SecretKey const& k, Asset const& asset, int64_t balance)
        {
            LedgerState ls(app->getLedgerStateRoot());
            auto line = loadTrustLine(ls, k.getPublicKey(), asset);
            REQUIRE(line);
            REQUIRE(line->getBalance() == balance);
        };
    auto loadAccountAndCheckBalance =
        [&] (SecretKey const& k, int64_t balance)
        {
            LedgerState ls(app->getLedgerStateRoot());
            auto account = stellar::loadAccount(ls, k.getPublicKey());
            REQUIRE(account);
            REQUIRE(account.getBalance() == balance);
        };

    SECTION("simple credit")
    {
        SECTION("credit sent to new account (no account error)")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    gateway.pay(getAccount("B").getPublicKey(), idr, 100),
                    ex_PAYMENT_NO_DESTINATION);
            });
        }

        // actual sendcredit
        SECTION("credit payment with no trust")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(gateway.pay(a1, idr, 100),
                                  ex_PAYMENT_NO_TRUST);
            });
        }

        SECTION("with trust")
        {
            a1.changeTrust(idr, 1000);
            gateway.pay(a1, idr, 100);

            loadTrustLineAndCheckBalance(a1, idr, 100);

            // create b1 account
            auto b1 = root.create("B", paymentAmount);

            b1.changeTrust(idr, 100);

            SECTION("positive")
            {
                for_all_versions(*app, [&] {
                    // first, send 40 from a1 to b1
                    a1.pay(b1, idr, 40);

                    loadTrustLineAndCheckBalance(a1, idr, 60);
                    loadTrustLineAndCheckBalance(b1, idr, 40);

                    // then, send back to the gateway
                    // the gateway does not have a trust line as it's the issuer
                    b1.pay(gateway, idr, 40);
                    loadTrustLineAndCheckBalance(b1, idr, 0);
                });
            }
            SECTION("missing issuer")
            {
                for_all_versions(*app, [&] {
                    gateway.merge(root);
                    // cannot send to an account that is not the issuer
                    REQUIRE_THROWS_AS(a1.pay(b1, idr, 40),
                                      ex_PAYMENT_NO_ISSUER);
                    // should be able to send back credits to issuer
                    a1.pay(gateway, idr, 75);
                    // cannot change the limit
                    REQUIRE_THROWS_AS(a1.changeTrust(idr, 25),
                                      ex_CHANGE_TRUST_NO_ISSUER);
                    a1.pay(gateway, idr, 25);
                    // and should be able to delete the trust line too
                    a1.changeTrust(idr, 0);
                });
            }
        }
    }
    SECTION("issuer large amounts")
    {
        for_all_versions(*app, [&] {
            a1.changeTrust(idr, INT64_MAX);
            gateway.pay(a1, idr, INT64_MAX);
            loadTrustLineAndCheckBalance(a1, idr, INT64_MAX);

            // send it all back
            a1.pay(gateway, idr, INT64_MAX);
            loadTrustLineAndCheckBalance(a1, idr, 0);

            {
                LedgerKey key(TRUSTLINE);
                key.trustLine().accountID = a1.getPublicKey();
                key.trustLine().asset = idr;
                LedgerState ls(app->getLedgerStateRoot());
                REQUIRE(ls.load(key));
            }
            REQUIRE(app->countTrustLines({1, INT32_MAX}) == 1);
            // Since a1 has a trustline, and there is only 1 trustline, we know
            // that gateway has no trustlines.
        });
    }
    SECTION("authorize flag")
    {
        for_all_versions(*app, [&] {
            uint32_t setFlags = AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;

            gateway.setOptions(nullptr, &setFlags, nullptr, nullptr, nullptr,
                               nullptr);

            a1.changeTrust(idr, trustLineLimit);

            REQUIRE_THROWS_AS(gateway.pay(a1, idr, trustLineStartingBalance),
                              ex_PAYMENT_NOT_AUTHORIZED);

            gateway.allowTrust(idr, a1);

            gateway.pay(a1, idr, trustLineStartingBalance);

            // send it all back
            gateway.denyTrust(idr, a1);

            REQUIRE_THROWS_AS(a1.pay(gateway, idr, trustLineStartingBalance),
                              ex_PAYMENT_SRC_NOT_AUTHORIZED);

            gateway.allowTrust(idr, a1);

            a1.pay(gateway, idr, trustLineStartingBalance);
        });
    }

    for_all_versions(*app, [&] {
        SECTION("send to self")
        {
            auto sendToSelf = root.create("send to self", minBalance2);

            SECTION("native")
            {
                SECTION("few")
                {
                    sendToSelf.pay(sendToSelf, 1);
                }
                SECTION("all")
                {
                    sendToSelf.pay(sendToSelf, minBalance2);
                }
                SECTION("more than have")
                {
                    sendToSelf.pay(sendToSelf, INT64_MAX);
                }
                loadAccountAndCheckBalance(sendToSelf, minBalance2 - txfee);
            }

            auto fakeCur = makeAsset(gateway, "fake");
            auto fakeWithFakeAccountCur =
                makeAsset(getAccount("fake account"), "fake");

            using Pay = std::function<void(Asset const&, int64_t)>;
            using Data = struct
            {
                std::string name;
                Asset asset;
                Pay payWithoutTrustline;
                Pay payWithTrustLine;
                Pay payWithTrustLineFull;
            };

            Pay payOk = [&sendToSelf](Asset const& asset, int64_t amount) {
                sendToSelf.pay(sendToSelf, asset, amount);
            };
            Pay payNoTrust = [&sendToSelf](Asset const& asset, int64_t amount) {
                REQUIRE_THROWS_AS(sendToSelf.pay(sendToSelf, asset, amount),
                                  ex_PAYMENT_NO_TRUST);
            };
            Pay payLineFull = [&sendToSelf](Asset const& asset,
                                            int64_t amount) {
                REQUIRE_THROWS_AS(sendToSelf.pay(sendToSelf, asset, amount),
                                  ex_PAYMENT_LINE_FULL);
            };
            Pay payNoIssuer = [&sendToSelf](Asset const& asset,
                                            int64_t amount) {
                REQUIRE_THROWS_AS(sendToSelf.pay(sendToSelf, asset, amount),
                                  ex_PAYMENT_NO_ISSUER);
            };

            // in ledger versions 1 and 2 each of these payment succeeds
            if (getCurrentLedgerVersion(app->getLedgerStateRoot()) < 3)
            {
                payNoTrust = payOk;
                payLineFull = payOk;
                payNoIssuer = payOk;
            }

            auto withoutTrustLine = std::vector<Data>{
                Data{"existing asset", idr, payNoTrust, payOk, payLineFull},
                Data{"non existing asset with existing issuer", fakeCur,
                     payNoTrust, payOk, payLineFull},
                Data{"non existing asset with non existing issuer",
                     fakeWithFakeAccountCur, payNoIssuer, payNoIssuer,
                     payNoIssuer}};

            for (auto const& data : withoutTrustLine)
            {
                SECTION(data.name)
                {
                    SECTION("without trustline")
                    {
                        data.payWithoutTrustline(data.asset, 1);

                        loadAccountAndCheckBalance(sendToSelf, minBalance2 - txfee);
                        LedgerState ls(app->getLedgerStateRoot());
                        REQUIRE(!loadTrustLine(ls, sendToSelf, data.asset));
                    }
                }
            }

            auto withTrustLine = withoutTrustLine;
            withTrustLine.resize(2);

            sendToSelf.changeTrust(idr, 1000);
            sendToSelf.changeTrust(fakeCur, 1000);
            REQUIRE_THROWS_AS(
                sendToSelf.changeTrust(fakeWithFakeAccountCur, 1000),
                ex_CHANGE_TRUST_NO_ISSUER);

            for (auto const& data : withTrustLine)
            {
                SECTION(data.name)
                {
                    SECTION("with trustline and 0 balance")
                    {
                        SECTION("few")
                        {
                            data.payWithTrustLine(data.asset, 1);
                        }
                        SECTION("all")
                        {
                            data.payWithTrustLine(data.asset, 1000);
                        }
                        SECTION("more than have")
                        {
                            data.payWithTrustLineFull(data.asset, 2000);
                        }
                        loadAccountAndCheckBalance(sendToSelf, minBalance2 - 4*txfee);
                        loadTrustLineAndCheckBalance(sendToSelf, data.asset, 0);
                    }

                    SECTION("with trustline and half balance")
                    {
                        gateway.pay(sendToSelf, data.asset, 500);

                        SECTION("few")
                        {
                            data.payWithTrustLine(data.asset, 1);
                        }
                        SECTION("to full")
                        {
                            data.payWithTrustLine(data.asset, 500);
                        }
                        SECTION("more than have")
                        {
                            data.payWithTrustLineFull(data.asset, 2000);
                        }
                        loadAccountAndCheckBalance(sendToSelf, minBalance2 - 4*txfee);
                        loadTrustLineAndCheckBalance(sendToSelf, data.asset, 500);
                    }

                    SECTION("with trustline and full balance")
                    {
                        gateway.pay(sendToSelf, data.asset, 1000);

                        SECTION("few")
                        {
                            data.payWithTrustLineFull(data.asset, 1);
                        }
                        SECTION("all")
                        {
                            data.payWithTrustLineFull(data.asset, 1000);
                        }
                        SECTION("more than have")
                        {
                            data.payWithTrustLineFull(data.asset, 2000);
                        }
                        loadAccountAndCheckBalance(sendToSelf, minBalance2 - 4*txfee);
                        loadTrustLineAndCheckBalance(sendToSelf, data.asset, 1000);
                    }
                }
            }
        }
    });

    int amount = 1;
    SECTION("fee less than base reserve")
    {
        SECTION("account has only base reserve + amount")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + 1);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee - "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee - 1);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_PAYMENT_UNDERFUNDED);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + one operation fee + "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee + 1);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees - "
                "two stroops")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount +
                                2 * txfee - 2);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees - "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount +
                                2 * txfee - 1);
            for_all_versions(*app,
                             [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + 2 * txfee);
            for_all_versions(*app,
                             [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }
    }
}

TEST_CASE("payment fees", "[tx][payment]")
{
    int amount = 1;

    SECTION("fee equal to base reserve")
    {
        auto cfg = getTestConfig(1);
        cfg.TESTING_UPGRADE_DESIRED_FEE = 100000000;

        VirtualClock clock;
        auto app = createTestApplication(clock, cfg);
        app->start();

        // set up world
        auto root = TestAccount::createRoot(*app);
        auto txfee = getCurrentTxFee(app->getLedgerStateRoot());

        SECTION("account has only base reserve + amount")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + 1);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee - "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee - 1);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_PAYMENT_UNDERFUNDED);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + one operation fee + "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee + 1);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees - "
                "two stroops")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount +
                                2 * txfee - 2);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees - "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount +
                                2 * txfee - 1);
            for_all_versions(*app,
                             [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + 2 * txfee);
            for_all_versions(*app,
                             [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }
    }

    SECTION("fee bigger than base reserve")
    {
        auto cfg = getTestConfig(1);
        cfg.TESTING_UPGRADE_DESIRED_FEE = 200000000;

        VirtualClock clock;
        auto app = createTestApplication(clock, cfg);
        app->start();

        // set up world
        auto root = TestAccount::createRoot(*app);
        auto txfee = getCurrentTxFee(app->getLedgerStateRoot());

        SECTION("account has only base reserve + amount")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + 1);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee - "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee - 1);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_PAYMENT_UNDERFUNDED);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + one operation fee + "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + txfee + 1);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees - "
                "two stroops")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount +
                                2 * txfee - 2);
            for_versions_to(8, *app, [&] {
                REQUIRE_THROWS_AS(payFrom.pay(root, 1),
                                  ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, *app,
                              [&] { REQUIRE_NOTHROW(payFrom.pay(root, 1)); });
        }

        SECTION("account has only base reserve + amount + two operation fees - "
                "one stroop")
        {
            auto payFrom = root.create(
                "pay-from", getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount +
                                2 * txfee - 1);
            for_all_versions(*app, [&] { payFrom.pay(root, 1); });
        }

        SECTION("account has only base reserve + amount + two operation fees")
        {
            auto payFrom = root.create(
                "pay-from",
                getCurrentMinBalance(app->getLedgerStateRoot(), 0) + amount + 2 * txfee);
            for_all_versions(*app, [&] { payFrom.pay(root, 1); });
        }
    }
}

TEST_CASE("single create account SQL", "[singlesql][paymentsql][hide]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    if (!force_sqlite)
        mode = Config::TESTDB_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        createTestApplication(clock, getTestConfig(0, mode));
    app->start();

    auto root = TestAccount::createRoot(*app);
    int64_t txfee = getCurrentTxFee(app->getLedgerStateRoot());
    const int64_t paymentAmount =
        getCurrentMinBalance(app->getLedgerStateRoot(), 1) + txfee * 10;

    {
        auto ctx = app->getDatabase().captureAndLogSQL("createAccount");
        auto a1 = root.create("A", paymentAmount);
    }
}
