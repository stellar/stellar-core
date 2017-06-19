// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

namespace
{

long operator*(long x, const Price& y)
{
    return x * y.n / y.d;
}

Price operator*(const Price& x, const Price& y)
{
    return Price{x.n * y.n, x.d * y.d};
}

template <typename T>
void
rotateRight(std::deque<T>& d)
{
    auto e = d.back();
    d.pop_back();
    d.push_front(e);
}

std::string
assetToString(const Asset& asset)
{
    auto r = std::string{};
    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        r = std::string{"XLM"};
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        assetCodeToStr(asset.alphaNum4().assetCode, r);
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        assetCodeToStr(asset.alphaNum12().assetCode, r);
        break;
    }
    return r;
};

std::string
assetPathToString(const std::deque<Asset>& assets)
{
    auto r = assetToString(assets[0]);
    for (auto i = assets.rbegin(); i != assets.rend(); i++)
    {
        r += " -> " + assetToString(*i);
    }
    return r;
};
}

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
    ApplicationEditableVersion app(clock, cfg);
    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);

    Asset xlmCur;

    int64_t txfee = app.getLedgerManager().getTxFee();

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        app.getLedgerManager().getMinBalance(2) + 10 * txfee;

    // minimum balance necessary to hold 2 trust lines and an offer
    const int64_t minBalance3 =
        app.getLedgerManager().getMinBalance(3) + 10 * txfee;

    const int64_t paymentAmount = minBalance2;

    // create an account
    auto a1 = root.create("A", paymentAmount);

    const int64_t morePayment = paymentAmount / 2;

    const int64_t assetMultiplier = 10000000;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000 * assetMultiplier;

    // sets up gateway account
    const int64_t gatewayPayment = minBalance2 + morePayment;
    auto gateway = root.create("gate", gatewayPayment);

    // sets up gateway2 account
    auto gateway2 = root.create("gate2", gatewayPayment);

    Asset idrCur = makeAsset(gateway, "IDR");
    Asset usdCur = makeAsset(gateway2, "USD");

    AccountFrame::pointer a1Account, rootAccount;
    rootAccount = loadAccount(root, app);
    a1Account = loadAccount(a1, app);
    REQUIRE(rootAccount->getMasterWeight() == 1);
    REQUIRE(rootAccount->getHighThreshold() == 0);
    REQUIRE(rootAccount->getLowThreshold() == 0);
    REQUIRE(rootAccount->getMediumThreshold() == 0);
    REQUIRE(a1Account->getBalance() == paymentAmount);
    REQUIRE(a1Account->getMasterWeight() == 1);
    REQUIRE(a1Account->getHighThreshold() == 0);
    REQUIRE(a1Account->getLowThreshold() == 0);
    REQUIRE(a1Account->getMediumThreshold() == 0);
    // root did 2 transactions at this point
    REQUIRE(rootAccount->getBalance() == (1000000000000000000 - paymentAmount -
                                          gatewayPayment * 2 - txfee * 3));

    SECTION("Create account")
    {
        SECTION("Success")
        {
            for_all_versions(app, [&]{
                auto b1 = root.create("B", app.getLedgerManager().getMinBalance(0));
                SECTION("Account already exists")
                {
                    REQUIRE_THROWS_AS(
                        root.create("B", app.getLedgerManager().getMinBalance(0)),
                        ex_CREATE_ACCOUNT_ALREADY_EXIST);
                }
            });
        }
        SECTION("Not enough funds (source)")
        {
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(gateway.create("B", gatewayPayment),
                                ex_CREATE_ACCOUNT_UNDERFUNDED);
            });
        }
        SECTION("Amount too small to create account")
        {
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(
                    root.create("B", app.getLedgerManager().getMinBalance(0) - 1),
                    ex_CREATE_ACCOUNT_LOW_RESERVE);
            });
        }
    }

    SECTION("a pays b, then a merge into b")
    {
        auto paymentAmount = 1000000;
        auto amount = app.getLedgerManager().getMinBalance(0) + paymentAmount;
        auto b1 = root.create("B", amount);

        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();

        auto txFrame = a1.tx(
            {payment(b1, 200), accountMerge(b1)});

        for_all_versions(app, [&]{
            auto res = applyCheck(txFrame, app);

            REQUIRE(!loadAccount(a1, app, false));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            REQUIRE(!loadAccount(a1, app, false));
            REQUIRE((a1Balance + b1Balance - txFrame->getFee()) ==
                    b1.getBalance());
        });
    }

    SECTION("a pays b, then b merge into a")
    {
        auto paymentAmount = 1000000;
        auto amount = app.getLedgerManager().getMinBalance(0) + paymentAmount;
        auto b1 = root.create("B", amount);

        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();

        auto txFrame = a1.tx({payment(b1, 200),
                              b1.op(accountMerge(a1))});
        txFrame->addSignature(b1);

        for_all_versions(app, [&]{
            auto res = applyCheck(txFrame, app);

            REQUIRE(loadAccount(a1, app));
            REQUIRE(!loadAccount(b1, app, false));
            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            REQUIRE(!loadAccount(b1, app, false));
            REQUIRE((a1Balance + b1Balance - txFrame->getFee()) ==
                    a1.getBalance());
        });
    }

    SECTION("merge then send")
    {
        auto b1 = root.create("B", app.getLedgerManager().getMinBalance(0));

        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();

        auto txFrame = a1.tx(
            {accountMerge(b1), payment(b1, 200)});

        for_versions_to(7, app, [&]{
            auto res = applyCheck(txFrame, app);

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(txFrame->getResultCode() == txINTERNAL_ERROR);

            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFee()) == a1.getBalance());
        });

        for_versions_from(8, app, [&]{
            auto res = applyCheck(txFrame, app);

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(txFrame->getResultCode() == txFAILED);

            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFee()) == a1.getBalance());
        });
    }

    SECTION("send XLM to an existing account")
    {
        for_all_versions(app, [&]{
            root.pay(a1, morePayment);

            AccountFrame::pointer a1Account2, rootAccount2;
            rootAccount2 = loadAccount(root, app);
            a1Account2 = loadAccount(a1, app);
            REQUIRE(a1Account2->getBalance() ==
                    a1Account->getBalance() + morePayment);

            // root did 2 transactions at this point
            REQUIRE(rootAccount2->getBalance() ==
                    (rootAccount->getBalance() - morePayment - txfee));
        });
    }

    SECTION("send XLM to a new account (no destination)")
    {
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(
                root.pay(
                    getAccount("B").getPublicKey(),
                    app.getLedgerManager().getCurrentLedgerHeader().baseReserve *
                        2),
                ex_PAYMENT_NO_DESTINATION);

            AccountFrame::pointer rootAccount2;
            rootAccount2 = loadAccount(root, app);
            REQUIRE(rootAccount2->getBalance() ==
                    (rootAccount->getBalance() - txfee));
        });
    }

    SECTION("rescue account (was below reserve)")
    {
        for_all_versions(app, [&]{
            int64 orgReserve = app.getLedgerManager().getMinBalance(0);

            auto b1 = root.create("B", orgReserve + 1000);

            // raise the reserve
            uint32 addReserve = 100000;
            app.getLedgerManager().getCurrentLedgerHeader().baseReserve +=
                addReserve;

            // verify that the account can't do anything
            auto tx = b1.tx({payment(root, 1)});
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(tx->getResultCode() == txINSUFFICIENT_BALANCE);

            // top up the account to unblock it
            int64 topUp = app.getLedgerManager().getMinBalance(0) - orgReserve;
            root.pay(b1, topUp);

            // payment goes through
            b1.pay(root, 1);
        });
    }

    SECTION("two payments, first breaking second")
    {
        int64 startingBalance = paymentAmount + 5 +
                                app.getLedgerManager().getMinBalance(0) +
                                txfee * 2;
        auto b1 = root.create("B", startingBalance);

        auto tx1 = b1.tx({payment(root, paymentAmount)});
        auto tx2 = b1.tx({payment(root, 6)});

        auto rootBalance = root.getBalance();

        for_versions_to(8, app, [&]{
            auto r = closeLedgerOn(app, 2, 1, 1, 2015, {tx1, tx2});
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txINSUFFICIENT_BALANCE);

            int64 expectedrootBalance = rootBalance + paymentAmount;
            int64 expectedb1Balance = app.getLedgerManager().getMinBalance(0) + 5;
            REQUIRE(expectedb1Balance == b1.getBalance());
            REQUIRE(expectedrootBalance == root.getBalance());
        });

        for_versions_from(9, app, [&]{
            auto r = closeLedgerOn(app, 2, 1, 1, 2015, {tx1, tx2});
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txFAILED);
            REQUIRE(r[1].first.result.result.results()[0].tr().paymentResult().code() == PAYMENT_UNDERFUNDED);

            int64 expectedrootBalance = rootBalance + paymentAmount;
            int64 expectedb1Balance = app.getLedgerManager().getMinBalance(0) + 5;
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
        auto createSourceAccount = TestAccount{app, getAccount("create")};
        auto balanceBefore = sourceAccount.getBalance();
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();

        auto tx = sourceAccount.tx({
            createAccount(createSourceAccount, createAmount),
            accountMerge(createSourceAccount),
            payment(sourceAccount, payAmount)
        });

        for_versions_to(7, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(!loadAccount(sourceAccount, app, false));
            REQUIRE(loadAccount(createSourceAccount, app));
            REQUIRE(createSourceAccount.getBalance() == amount - tx->getFee());

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[0].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - createAmount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[2].tr().paymentResult().code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(!loadAccount(createSourceAccount, app, false));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[0].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - createAmount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
        });
    }

    SECTION("create, merge, pay, 3 accounts")
    {
        auto amount = 300000000000000;
        auto createAmount = 500000000;
        auto payAmount = 200000000;
        auto sourceAccount = root.create("source", amount);
        auto createSourceAccount = TestAccount{app, getAccount("create")};
        auto payAccount = root.create("pay", amount);
        auto balanceBefore = sourceAccount.getBalance() + payAccount.getBalance();
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();

        auto tx = sourceAccount.tx({
            createAccount(createSourceAccount, createAmount),
            accountMerge(createSourceAccount),
            payment(payAccount, payAmount)
        });

        for_versions_to(7, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(!loadAccount(createSourceAccount, app, false));
            REQUIRE(loadAccount(payAccount, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(payAccount.getBalance() == amount);

            REQUIRE(tx->getResult().result.code() == txINTERNAL_ERROR);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(!loadAccount(createSourceAccount, app, false));
            REQUIRE(loadAccount(payAccount, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(payAccount.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[0].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - createAmount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
        });
    }

    SECTION("pay, merge, create, pay, self")
    {
        auto amount = 300000000000000;
        auto pay1Amount = 100000000;
        auto createAmount = 500000000;
        auto pay2Amount = 200000000;
        auto sourceAccount = root.create("source", amount);
        auto payAndMergeDestination = root.create("payAndMerge", amount);
        auto balanceBefore = sourceAccount.getBalance()
                + payAndMergeDestination.getBalance();
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto payAndMergeDestinationSeqNum = payAndMergeDestination.getLastSequenceNumber();

        auto tx = sourceAccount.tx({
            payAndMergeDestination.op(payment(payAndMergeDestination, pay1Amount)),
            accountMerge(payAndMergeDestination),
            payAndMergeDestination.op(createAccount(sourceAccount, createAmount)),
            payAndMergeDestination.op(payment(payAndMergeDestination, pay2Amount))
        });
        tx->addSignature(payAndMergeDestination);

        for_versions_to(7, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(payAndMergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == createAmount);
            REQUIRE(payAndMergeDestination.getBalance() == amount + amount - createAmount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() == payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(payAndMergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == createAmount);
            REQUIRE(payAndMergeDestination.getBalance() == amount + amount - createAmount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() == payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
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
        auto payAndMergeDestinationSeqNum = payAndMergeDestination.getLastSequenceNumber();

        auto tx = sourceAccount.tx({
            payment(payAndMergeDestination, pay1Amount),
            accountMerge(payAndMergeDestination),
            payAndMergeDestination.op(createAccount(sourceAccount, createAmount)),
            payment(payAndMergeDestination, pay2Amount)
        });
        tx->addSignature(payAndMergeDestination);

        for_versions_to(7, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(payAndMergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - pay1Amount - pay2Amount - tx->getFee());
            REQUIRE(payAndMergeDestination.getBalance() == amount + amount + pay2Amount - tx->getFee() - createAmount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() == payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(payAndMergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == createAmount - pay2Amount);
            REQUIRE(payAndMergeDestination.getBalance() == amount + amount + pay2Amount - tx->getFee() - createAmount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() == payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
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
        auto payAndMergeDestinationSeqNum = payAndMergeDestination.getLastSequenceNumber();

        auto tx = sourceAccount.tx({
            payment(payAndMergeDestination, pay1Amount),
            accountMerge(payAndMergeDestination),
            secondSourceAccount.op(createAccount(sourceAccount, createAmount)),
            payment(payAndMergeDestination, pay2Amount)
        });
        tx->addSignature(secondSourceAccount);

        for_versions_to(7, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(secondSourceAccount, app));
            REQUIRE(loadAccount(payAndMergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - pay1Amount - pay2Amount - tx->getFee());
            REQUIRE(secondSourceAccount.getBalance() == amount - createAmount);
            REQUIRE(payAndMergeDestination.getBalance() == amount + amount + pay2Amount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(secondSourceAccount.loadSequenceNumber() == secondSourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() == payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(secondSourceAccount, app));
            REQUIRE(loadAccount(payAndMergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == createAmount - pay2Amount);
            REQUIRE(secondSourceAccount.getBalance() == amount - createAmount);
            REQUIRE(payAndMergeDestination.getBalance() == amount + amount + pay2Amount - tx->getFee());
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum);
            REQUIRE(secondSourceAccount.loadSequenceNumber() == secondSourceSeqNum);
            REQUIRE(payAndMergeDestination.loadSequenceNumber() == payAndMergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - pay1Amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
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
        auto createDestination = TestAccount{app, getAccount("createDestination")};
        auto payDestination = root.create("pay", amount);
        auto sourceSeqNum = sourceAccount.getLastSequenceNumber();
        auto createSourceSeqNum = createSource.getLastSequenceNumber();
        auto payDestinationSeqNum = payDestination.getLastSequenceNumber();

        auto tx = sourceAccount.tx({
            createSource.op(createAccount(createDestination, create1Amount)),
            createDestination.op(pathPayment(payDestination,
                xlmCur, payAmount, xlmCur, payAmount, {})),
            payDestination.op(accountMerge(createSource)),
            createSource.op(createAccount(payDestination, create2Amount)),
        });
        tx->addSignature(createSource);
        tx->addSignature(createDestination);
        tx->addSignature(payDestination);

        for_all_versions(app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(createSource, app));
            REQUIRE(loadAccount(createDestination, app));
            REQUIRE(loadAccount(payDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(createSource.getBalance() == amount + amount - create1Amount - create2Amount + payAmount);
            REQUIRE(createDestination.getBalance() == create1Amount - payAmount);
            REQUIRE(payDestination.getBalance() == create2Amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(createSource.loadSequenceNumber() == createSourceSeqNum);
            REQUIRE(payDestination.loadSequenceNumber() == payDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[0].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PATH_PAYMENT);
            REQUIRE(tx->getResult().result.results()[1].tr().pathPaymentResult().code() == PATH_PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[2].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].tr().accountMergeResult().sourceAccountBalance() == amount + payAmount);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[3].tr().createAccountResult().code() == CREATE_ACCOUNT_SUCCESS);
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

        auto tx = sourceAccount.tx({
            payment(sourceAccount, pay1Amount),
            accountMerge(mergeDestination),
            payment(sourceAccount, pay2Amount),
            accountMerge(mergeDestination)
        });

        for_versions_to(4, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(!loadAccount(sourceAccount, app, false));
            REQUIRE(loadAccount(mergeDestination, app));
            REQUIRE(mergeDestination.getBalance() == amount + amount + amount - tx->getFee() - tx->getFee());
            REQUIRE(mergeDestination.loadSequenceNumber() == mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[2].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
        });

        for_versions(5, 7, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(mergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() == mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[2].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().accountMergeResult().code() == ACCOUNT_MERGE_NO_ACCOUNT);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(mergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() == mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
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

        auto tx = sourceAccount.tx({
            payment(sourceAccount, pay1Amount),
            payment(sourceAccount, pay1Amount),
            payment(sourceAccount, pay1Amount),
            payment(sourceAccount, pay1Amount),
            payment(sourceAccount, pay1Amount),
            payment(sourceAccount, pay1Amount),
            accountMerge(mergeDestination),
            payment(sourceAccount, pay2Amount),
            payment(sourceAccount, pay2Amount),
            accountMerge(mergeDestination)
        });

        for_versions_to(4, app, [&]{
            REQUIRE(applyCheck(tx, app));
            REQUIRE(!loadAccount(sourceAccount, app, false));
            REQUIRE(loadAccount(mergeDestination, app));
            REQUIRE(mergeDestination.getBalance() == amount + amount + amount - tx->getFee() - tx->getFee());
            REQUIRE(mergeDestination.loadSequenceNumber() == mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txSUCCESS);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[1].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[2].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[4].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[4].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[4].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[5].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[5].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[5].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[6].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[7].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[7].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[7].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[8].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[8].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[8].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[9].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[9].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[9].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
        });

        for_versions(5, 7, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(mergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() == mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[1].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[2].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[4].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[4].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[4].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[5].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[5].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[5].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[6].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[7].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[7].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[7].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[8].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[8].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[8].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[9].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[9].tr().accountMergeResult().code() == ACCOUNT_MERGE_NO_ACCOUNT);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(tx, app));
            REQUIRE(loadAccount(sourceAccount, app));
            REQUIRE(loadAccount(mergeDestination, app));
            REQUIRE(sourceAccount.getBalance() == amount - tx->getFee());
            REQUIRE(mergeDestination.getBalance() == amount);
            REQUIRE(sourceAccount.loadSequenceNumber() == sourceSeqNum + 1);
            REQUIRE(mergeDestination.loadSequenceNumber() == mergeDestinationSeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[0].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[1].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[2].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[3].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[3].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[3].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[4].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[4].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[4].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[5].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[5].tr().type() == PAYMENT);
            REQUIRE(tx->getResult().result.results()[5].tr().paymentResult().code() == PAYMENT_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[6].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[6].tr().accountMergeResult().sourceAccountBalance() == amount - tx->getFee());
            REQUIRE(tx->getResult().result.results()[7].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[8].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[9].code() == opNO_ACCOUNT);
        });
    }

    SECTION("simple credit")
    {
        SECTION("credit sent to new account (no account error)")
        {
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(
                    gateway.pay(getAccount("B").getPublicKey(), idrCur, 100),
                    ex_PAYMENT_NO_DESTINATION);
            });
        }

        // actual sendcredit
        SECTION("credit payment with no trust")
        {
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(gateway.pay(a1, idrCur, 100),
                                ex_PAYMENT_NO_TRUST);
            });
        }

        SECTION("with trust")
        {
            a1.changeTrust(idrCur, 1000);
            gateway.pay(a1, idrCur, 100);

            TrustFrame::pointer line;
            line = loadTrustLine(a1, idrCur, app);
            REQUIRE(line->getBalance() == 100);

            // create b1 account
            auto b1 = root.create("B", paymentAmount);

            b1.changeTrust(idrCur, 100);

            SECTION("positive")
            {
                for_all_versions(app, [&]{
                    // first, send 40 from a1 to b1
                    a1.pay(b1, idrCur, 40);

                    line = loadTrustLine(a1, idrCur, app);
                    REQUIRE(line->getBalance() == 60);
                    line = loadTrustLine(b1, idrCur, app);
                    REQUIRE(line->getBalance() == 40);

                    // then, send back to the gateway
                    // the gateway does not have a trust line as it's the issuer
                    b1.pay(gateway, idrCur, 40);
                    line = loadTrustLine(b1, idrCur, app);
                    REQUIRE(line->getBalance() == 0);
                });
            }
            SECTION("missing issuer")
            {
                for_all_versions(app, [&]{
                    gateway.merge(root);
                    // cannot send to an account that is not the issuer
                    REQUIRE_THROWS_AS(a1.pay(b1, idrCur, 40), ex_PAYMENT_NO_ISSUER);
                    // should be able to send back credits to issuer
                    a1.pay(gateway, idrCur, 75);
                    // cannot change the limit
                    REQUIRE_THROWS_AS(a1.changeTrust(idrCur, 25),
                                    ex_CHANGE_TRUST_NO_ISSUER);
                    a1.pay(gateway, idrCur, 25);
                    // and should be able to delete the trust line too
                    a1.changeTrust(idrCur, 0);
                });
            }
        }
    }
    SECTION("issuer large amounts")
    {
        for_all_versions(app, [&]{
            a1.changeTrust(idrCur, INT64_MAX);
            gateway.pay(a1, idrCur, INT64_MAX);
            TrustFrame::pointer line;
            line = loadTrustLine(a1, idrCur, app);
            REQUIRE(line->getBalance() == INT64_MAX);

            // send it all back
            a1.pay(gateway, idrCur, INT64_MAX);
            line = loadTrustLine(a1, idrCur, app);
            REQUIRE(line->getBalance() == 0);

            std::vector<TrustFrame::pointer> gwLines;
            TrustFrame::loadLines(gateway.getPublicKey(), gwLines,
                                app.getDatabase());
            REQUIRE(gwLines.size() == 0);
        });
    }
    SECTION("authorize flag")
    {
        for_all_versions(app, [&]{
            uint32_t setFlags = AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;

            gateway.setOptions(nullptr, &setFlags, nullptr, nullptr, nullptr,
                               nullptr);

            a1.changeTrust(idrCur, trustLineLimit);

            REQUIRE_THROWS_AS(gateway.pay(a1, idrCur, trustLineStartingBalance),
                            ex_PAYMENT_NOT_AUTHORIZED);

            gateway.allowTrust(idrCur, a1);

            gateway.pay(a1, idrCur, trustLineStartingBalance);

            // send it all back
            gateway.denyTrust(idrCur, a1);

            REQUIRE_THROWS_AS(a1.pay(gateway, idrCur, trustLineStartingBalance),
                            ex_PAYMENT_SRC_NOT_AUTHORIZED);

            gateway.allowTrust(idrCur, a1);

            a1.pay(gateway, idrCur, trustLineStartingBalance);
        });
    }

    SECTION("path payment to self XLM")
    {
        auto a1Balance = a1.getBalance();
        auto amount = a1Balance / 10;

        for_versions_to(7, app, [&]{
            a1.pay(a1, xlmCur, amount, xlmCur, amount, {});
            REQUIRE(a1.getBalance() == a1Balance - amount - txfee);
        });

        for_versions_from(8, app, [&]{
            a1.pay(a1, xlmCur, amount, xlmCur, amount, {});
            REQUIRE(a1.getBalance() == a1Balance - txfee);
        });
    }

    SECTION("path payment to self asset")
    {
        auto curBalance = 10000;
        auto amount = curBalance / 10;

        a1.changeTrust(idrCur, 2 * curBalance);
        gateway.pay(a1, idrCur, curBalance);

        for_all_versions(app, [&]{
            a1.pay(a1, idrCur, amount, idrCur, amount, {});
            auto trustLine = loadTrustLine(a1, idrCur, app);
            REQUIRE(trustLine->getBalance() == curBalance);
        });
    }

    for_all_versions(app, [&]{
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
                auto account = loadAccount(sendToSelf, app);
                REQUIRE(account->getBalance() == minBalance2 - txfee);
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

            Pay payOk = [&sendToSelf](Asset const& asset, int amount) {
                sendToSelf.pay(sendToSelf, asset, amount);
            };
            Pay payNoTrust = [&sendToSelf](Asset const& asset, int amount) {
                REQUIRE_THROWS_AS(sendToSelf.pay(sendToSelf, asset, amount),
                                    ex_PAYMENT_NO_TRUST);
            };
            Pay payLineFull = [&sendToSelf](Asset const& asset,
                                            int amount) {
                REQUIRE_THROWS_AS(sendToSelf.pay(sendToSelf, asset, amount),
                                    ex_PAYMENT_LINE_FULL);
            };
            Pay payNoIssuer = [&sendToSelf](Asset const& asset,
                                            int amount) {
                REQUIRE_THROWS_AS(sendToSelf.pay(sendToSelf, asset, amount),
                                    ex_PAYMENT_NO_ISSUER);
            };

            // in ledger versions 1 and 2 each of these payment succeeds
            if (app.getLedgerManager().getCurrentLedgerVersion() < 3)
            {
                payNoTrust = payOk;
                payLineFull = payOk;
                payNoIssuer = payOk;
            }

            auto withoutTrustLine = std::vector<Data>{
                Data{"existing asset", idrCur, payNoTrust, payOk,
                        payLineFull},
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

                        auto account = loadAccount(sendToSelf, app);
                        REQUIRE(account->getBalance() ==
                                minBalance2 - txfee);
                        REQUIRE(!loadTrustLine(sendToSelf, data.asset, app,
                                                false));
                    }
                }
            }

            auto withTrustLine = withoutTrustLine;
            withTrustLine.resize(2);

            sendToSelf.changeTrust(idrCur, 1000);
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
                        auto account = loadAccount(sendToSelf, app);
                        REQUIRE(account->getBalance() ==
                                minBalance2 - 4 * txfee);
                        auto trustline = loadTrustLine(
                            sendToSelf, data.asset, app, true);
                        REQUIRE(trustline->getBalance() == 0);
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
                        auto account = loadAccount(sendToSelf, app);
                        REQUIRE(account->getBalance() ==
                                minBalance2 - 4 * txfee);
                        auto trustline = loadTrustLine(
                            sendToSelf, data.asset, app, true);
                        REQUIRE(trustline->getBalance() == 500);
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
                        auto account = loadAccount(sendToSelf, app);
                        REQUIRE(account->getBalance() ==
                                minBalance2 - 4 * txfee);
                        auto trustline = loadTrustLine(
                            sendToSelf, data.asset, app, true);
                        REQUIRE(trustline->getBalance() == 1000);
                    }
                }
            }
        }

        SECTION("payment through path")
        {
            SECTION("send XLM with path (not enough offers)")
            {
                REQUIRE_THROWS_AS(gateway.pay(a1, idrCur, morePayment * 10,
                                                xlmCur, morePayment, {}),
                                    ex_PATH_PAYMENT_TOO_FEW_OFFERS);
            }

            // setup a1
            a1.changeTrust(usdCur, trustLineLimit);
            a1.changeTrust(idrCur, trustLineLimit);

            gateway2.pay(a1, usdCur, trustLineStartingBalance);

            // add a couple offers in the order book

            OfferFrame::pointer offer;

            const Price usdPriceOffer(2, 1);

            // offer is sell 100 IDR for 200 USD ; buy USD @ 2.0 = sell IRD
            // @ 0.5

            auto b1 = root.create("B", minBalance3 + 10000);
            b1.changeTrust(usdCur, trustLineLimit);
            b1.changeTrust(idrCur, trustLineLimit);
            gateway.pay(b1, idrCur, trustLineStartingBalance);

            auto offerB1 = b1.manageOffer(0, idrCur, usdCur, usdPriceOffer,
                                            100 * assetMultiplier);

            // setup "c1"
            auto c1 = root.create("C", minBalance3 + 10000);

            c1.changeTrust(usdCur, trustLineLimit);
            c1.changeTrust(idrCur, trustLineLimit);

            gateway.pay(c1, idrCur, trustLineStartingBalance);

            // offer is sell 100 IDR for 150 USD ; buy USD @ 1.5 = sell IRD
            // @ 0.66
            auto offerC1 = c1.manageOffer(0, idrCur, usdCur, Price(3, 2),
                                            100 * assetMultiplier);

            // at this point:
            // a1 holds (0, IDR) (trustLineStartingBalance, USD)
            // b1 holds (trustLineStartingBalance, IDR) (0, USD)
            // c1 holds (trustLineStartingBalance, IDR) (0, USD)
            SECTION("send with path (over sendmax)")
            {
                // A1: try to send 100 IDR to B1
                // using 149 USD

                REQUIRE_THROWS_AS(a1.pay(b1, usdCur, 149 * assetMultiplier,
                                            idrCur, 100 * assetMultiplier, {}),
                                    ex_PATH_PAYMENT_OVER_SENDMAX);
            }

            SECTION("send with path (success)")
            {
                // A1: try to send 125 IDR to B1 using USD
                // should cost 150 (C's offer taken entirely) +
                //  50 (1/4 of B's offer)=200 USD

                auto res = a1.pay(b1, usdCur, 250 * assetMultiplier, idrCur,
                                    125 * assetMultiplier, {});

                auto const& multi = res.success();

                REQUIRE(multi.offers.size() == 2);

                TrustFrame::pointer line;

                // C1
                // offer was taken
                REQUIRE(multi.offers[0].offerID == offerC1);
                REQUIRE(!c1.hasOffer(offerC1));
                line = loadTrustLine(c1, idrCur, app);
                checkAmounts(line->getBalance(), trustLineStartingBalance -
                                                        100 * assetMultiplier);
                line = loadTrustLine(c1, usdCur, app);
                checkAmounts(line->getBalance(), 150 * assetMultiplier);

                // B1
                auto const& b1Res = multi.offers[1];
                REQUIRE(b1Res.offerID == offerB1);
                auto offer = b1.loadOffer(offerB1);
                OfferEntry const& oe = offer->getOffer();
                REQUIRE(b1Res.sellerID == b1.getPublicKey());
                checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
                checkAmounts(oe.amount, 75 * assetMultiplier);
                line = loadTrustLine(b1, idrCur, app);
                // 125 where sent, 25 were consumed by B's offer
                checkAmounts(line->getBalance(),
                                trustLineStartingBalance +
                                    (125 - 25) * assetMultiplier);
                line = loadTrustLine(b1, usdCur, app);
                checkAmounts(line->getBalance(), 50 * assetMultiplier);

                // A1
                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(line->getBalance(), 0);
                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(line->getBalance(), trustLineStartingBalance -
                                                        200 * assetMultiplier);
            }

            SECTION("missing issuer")
            {
                SECTION("dest is standard account")
                {
                    SECTION("last")
                    {
                        // gateway issued idrCur
                        gateway.merge(root);

                        REQUIRE_THROWS_AS(
                            a1.pay(b1, usdCur, 250 * assetMultiplier,
                                    idrCur, 125 * assetMultiplier, {},
                                    &idrCur),
                            ex_PATH_PAYMENT_NO_ISSUER);
                    }
                    SECTION("first")
                    {
                        // gateway2 issued usdCur
                        gateway2.merge(root);

                        REQUIRE_THROWS_AS(
                            a1.pay(b1, usdCur, 250 * assetMultiplier,
                                    idrCur, 125 * assetMultiplier, {},
                                    &usdCur),
                            ex_PATH_PAYMENT_NO_ISSUER);
                    }
                    SECTION("mid")
                    {
                        SecretKey missing = getAccount("missing");
                        Asset btcCur = makeAsset(missing, "BTC");
                        REQUIRE_THROWS_AS(
                            a1.pay(b1, usdCur, 250 * assetMultiplier,
                                    idrCur, 125 * assetMultiplier, {btcCur},
                                    &btcCur),
                            ex_PATH_PAYMENT_NO_ISSUER);
                    }
                }
                SECTION("dest is issuer")
                {
                    // single currency payment already covered elsewhere
                    // only one negative test:
                    SECTION("cannot take offers on the way")
                    {
                        // gateway issued idrCur
                        gateway.merge(root);

                        REQUIRE_THROWS_AS(
                            a1.pay(gateway, usdCur, 250 * assetMultiplier,
                                    idrCur, 125 * assetMultiplier, {}),
                            ex_PATH_PAYMENT_NO_DESTINATION);
                    }
                }
            }

            SECTION("send with path (takes own offer)")
            {
                // raise A1's balance by what we're trying to send
                root.pay(a1, 100 * assetMultiplier);

                // offer is sell 100 USD for 100 XLM
                a1.manageOffer(0, usdCur, xlmCur, Price(1, 1),
                                100 * assetMultiplier);

                // A1: try to send 100 USD to B1 using XLM

                REQUIRE_THROWS_AS(a1.pay(b1, xlmCur, 100 * assetMultiplier,
                                            usdCur, 100 * assetMultiplier, {}),
                                    ex_PATH_PAYMENT_OFFER_CROSS_SELF);
            }

            SECTION("send with path (offer participant reaching limit)")
            {
                // make it such that C can only receive 120 USD (4/5th of
                // offerC)
                c1.changeTrust(usdCur, 120 * assetMultiplier);

                // A1: try to send 105 IDR to B1 using USD
                // cost 120 (C's offer maxed out at 4/5th of published
                // amount)
                //  50 (1/4 of B's offer)=170 USD

                auto res = a1.pay(b1, usdCur, 400 * assetMultiplier, idrCur,
                                    105 * assetMultiplier, {});

                auto& multi = res.success();

                REQUIRE(multi.offers.size() == 2);

                TrustFrame::pointer line;

                // C1
                // offer was taken
                REQUIRE(multi.offers[0].offerID == offerC1);
                REQUIRE(!c1.hasOffer(offerC1));
                line = loadTrustLine(c1, idrCur, app);
                checkAmounts(line->getBalance(), trustLineStartingBalance -
                                                        80 * assetMultiplier);
                line = loadTrustLine(c1, usdCur, app);
                checkAmounts(line->getBalance(),
                                line->getTrustLine().limit);

                // B1
                auto const& b1Res = multi.offers[1];
                REQUIRE(b1Res.offerID == offerB1);
                auto offer = b1.loadOffer(offerB1);
                OfferEntry const& oe = offer->getOffer();
                REQUIRE(b1Res.sellerID == b1.getPublicKey());
                checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
                checkAmounts(oe.amount, 75 * assetMultiplier);
                line = loadTrustLine(b1, idrCur, app);
                // 105 where sent, 25 were consumed by B's offer
                checkAmounts(line->getBalance(),
                                trustLineStartingBalance +
                                    (105 - 25) * assetMultiplier);
                line = loadTrustLine(b1, usdCur, app);
                checkAmounts(line->getBalance(), 50 * assetMultiplier);

                // A1
                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(line->getBalance(), 0);
                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(line->getBalance(), trustLineStartingBalance -
                                                        170 * assetMultiplier);
            }
            SECTION("missing trust line")
            {
                // modify C's trustlines to invalidate C's offer
                // * C's offer should be deleted
                // sell 100 IDR for 200 USD
                // * B's offer 25 IDR by 50 USD

                auto checkBalances = [&]() {
                    auto res = a1.pay(b1, usdCur, 200 * assetMultiplier,
                                        idrCur, 25 * assetMultiplier, {});

                    auto& multi = res.success();

                    REQUIRE(multi.offers.size() == 2);

                    TrustFrame::pointer line;

                    // C1
                    // offer was deleted
                    REQUIRE(multi.offers[0].offerID == offerC1);
                    REQUIRE(multi.offers[0].amountSold == 0);
                    REQUIRE(multi.offers[0].amountBought == 0);
                    REQUIRE(!c1.hasOffer(offerC1));

                    // B1
                    auto const& b1Res = multi.offers[1];
                    REQUIRE(b1Res.offerID == offerB1);
                    auto offer = b1.loadOffer(offerB1);
                    OfferEntry const& oe = offer->getOffer();
                    REQUIRE(b1Res.sellerID == b1.getPublicKey());
                    checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
                    checkAmounts(oe.amount, 75 * assetMultiplier);
                    line = loadTrustLine(b1, idrCur, app);
                    // As B was the sole participant in the exchange, the
                    // IDR
                    // balance should not have changed
                    checkAmounts(line->getBalance(),
                                    trustLineStartingBalance);
                    line = loadTrustLine(b1, usdCur, app);
                    // but 25 USD cost 50 USD to send
                    checkAmounts(line->getBalance(), 50 * assetMultiplier);

                    // A1
                    line = loadTrustLine(a1, idrCur, app);
                    checkAmounts(line->getBalance(), 0);
                    line = loadTrustLine(a1, usdCur, app);
                    checkAmounts(line->getBalance(),
                                    trustLineStartingBalance -
                                        50 * assetMultiplier);
                };

                SECTION("deleted selling line")
                {
                    c1.pay(gateway, idrCur, trustLineStartingBalance);
                    c1.changeTrust(idrCur, 0);

                    checkBalances();
                }

                SECTION("deleted buying line")
                {
                    c1.changeTrust(usdCur, 0);
                    checkBalances();
                }
            }
        }

        SECTION("path payment with rounding errors")
        {
            auto issuer = root.create("issuer", 5999999400);
            auto source = root.create("source", 1989999000);
            auto destination = root.create("destination", 499999700);
            auto seller = root.create("seller", 20999999300);

            auto cnyCur = makeAsset(issuer, "CNY");
            destination.changeTrust(cnyCur, INT64_MAX);
            seller.changeTrust(cnyCur, 100000 * assetMultiplier);

            issuer.pay(seller, cnyCur, 170 * assetMultiplier);
            auto price = Price{2000, 29};
            auto offerId =
                seller.manageOffer(0, cnyCur, xlmCur, price, 145000000);

            auto path = std::vector<Asset>{};
            if (app.getLedgerManager().getCurrentLedgerVersion() <= 2)
            {
                // bug, it should succeed
                REQUIRE_THROWS_AS(source.pay(destination, xlmCur,
                                                1382068965, cnyCur,
                                                2 * assetMultiplier, path),
                                    ex_PATH_PAYMENT_TOO_FEW_OFFERS);
            }
            else
            {
                source.pay(destination, xlmCur, 1382068965, cnyCur,
                            2 * assetMultiplier, path);
                auto sellerOffer = seller.loadOffer(offerId);
                REQUIRE(sellerOffer->getPrice() == price);
                REQUIRE(sellerOffer->getAmount() ==
                        145000000 - 2 * assetMultiplier);
                REQUIRE(sellerOffer->getBuying().type() ==
                        ASSET_TYPE_NATIVE);
                REQUIRE(sellerOffer->getSelling().alphaNum4().assetCode ==
                        cnyCur.alphaNum4().assetCode);

                auto sourceAccount = loadAccount(source, app);
                // 1379310345 = round up(20000000 * price)
                REQUIRE(sourceAccount->getBalance() ==
                        1989999000 - 100 - 1379310345);

                auto sellerLine = loadTrustLine(seller, cnyCur, app);
                REQUIRE(sellerLine->getBalance() == 168 * assetMultiplier);

                auto destinationLine =
                    loadTrustLine(destination, cnyCur, app);
                REQUIRE(destinationLine->getBalance() ==
                        2 * assetMultiplier);
            }
        }

        SECTION("path with bogus offer, bogus offer shows on offers trail")
        {
            auto paymentToReceive = 24 * assetMultiplier;
            auto offerSize = paymentToReceive / 2;
            auto initialBalance = app.getLedgerManager().getMinBalance(10) +
                                    txfee * 10 + 100 * assetMultiplier;
            auto mm = root.create("mm", initialBalance);
            auto source = root.create("source", initialBalance);
            auto destination = root.create("destination", initialBalance);
            mm.changeTrust(idrCur, trustLineLimit);
            mm.changeTrust(usdCur, trustLineLimit);
            destination.changeTrust(idrCur, trustLineLimit);
            gateway.pay(mm, idrCur, 100 * assetMultiplier);
            gateway2.pay(mm, usdCur, 100 * assetMultiplier);

            auto idrCurCheapOfferID =
                mm.manageOffer(0, idrCur, usdCur, Price{3, 12}, offerSize);
            auto idrCurMidBogusOfferID =
                mm.manageOffer(0, idrCur, usdCur, Price{4, 12}, 1);
            auto idrCurExpensiveOfferID =
                mm.manageOffer(0, idrCur, usdCur, Price{6, 12}, offerSize);
            auto usdCurOfferID = mm.manageOffer(0, usdCur, xlmCur,
                                                Price{1, 2}, 2 * offerSize);

            auto path = std::vector<Asset>{xlmCur, usdCur};
            auto res = source.pay(destination, xlmCur, 8 * paymentToReceive,
                                    idrCur, paymentToReceive, path);

            auto const& offers = res.success().offers;
            REQUIRE(offers.size() == 4);
            REQUIRE(std::find_if(std::begin(offers), std::end(offers),
                                    [&](ClaimOfferAtom const& x) {
                                        return x.offerID == idrCurCheapOfferID;
                                    }) != std::end(offers));
            REQUIRE(std::find_if(std::begin(offers), std::end(offers),
                                    [&](ClaimOfferAtom const& x) {
                                        return x.offerID ==
                                            idrCurMidBogusOfferID;
                                    }) != std::end(offers));
            REQUIRE(std::find_if(std::begin(offers), std::end(offers),
                                    [&](ClaimOfferAtom const& x) {
                                        return x.offerID ==
                                            idrCurExpensiveOfferID;
                                    }) != std::end(offers));
            REQUIRE(std::find_if(std::begin(offers), std::end(offers),
                                    [&](ClaimOfferAtom const& x) {
                                        return x.offerID == usdCurOfferID;
                                    }) != std::end(offers));
        }

        SECTION("path payment with cycle")
        {
            // Create 3 different cycles.
            // First cycle involves 3 transaction in which buying price is
            // always half - so sender buys 8 times as much XLM as he/she
            // sells (arbitrage).
            // Second cycle involves 3 transaction in which buying price is
            // always two - so sender buys 8 times as much XLM as he/she
            // sells (anti-arbitrage).
            // Thanks to send max option this transaction is rejected.
            // Third cycle is similar to second, but send max is set to a
            // high value, so transaction proceeds even if it makes sender
            // lose a lot of XLM.

            // Each cycle is created in 3 variants (to check if behavior
            // does not depend of nativeness of asset):
            // * XLM -> USD -> IDR -> XLM
            // * USD -> IDR -> XLM -> USD
            // * IDR -> XLM -> USD -> IDR
            // To create variants, rotateRight() function is used on
            // accounts, offers and assets -
            // it greatly simplified index calculation in the code.

            auto paymentAmount =
                10 * assetMultiplier; // amount of money that 'destination'
                                        // account will receive
            auto offerAmount = 8 * paymentAmount; // amount of money in
                                                    // offer required to pass
                                                    // - needs 8x of payment
                                                    // for anti-arbitrage case
            auto initialBalance =
                2 * offerAmount; // we need twice as much money as in the
                                    // offer because of Price{2, 1} that is
                                    // used in one case
            auto txFee = app.getLedgerManager().getTxFee();

            auto assets = std::deque<Asset>{xlmCur, usdCur, idrCur};
            auto pathSize = assets.size();
            auto accounts = std::deque<TestAccount>{};

            auto setupAccount = [&](const std::string& name) {
                // setup account with required trustlines and money both in
                // native and assets
                auto account = root.create(name, initialBalance);
                account.changeTrust(idrCur, trustLineLimit);
                gateway.pay(account, idrCur, initialBalance);
                account.changeTrust(usdCur, trustLineLimit);
                gateway2.pay(account, usdCur, initialBalance);

                return account;
            };

            auto validateAccountAsset = [&](const TestAccount& account,
                                            int assetIndex, int difference,
                                            int feeCount) {
                if (assets[assetIndex].type() == ASSET_TYPE_NATIVE)
                {
                    REQUIRE(account.getBalance() ==
                            initialBalance + difference - feeCount * txFee);
                }
                else
                {
                    REQUIRE(loadTrustLine(account, assets[assetIndex], app)
                                ->getBalance() ==
                            initialBalance + difference);
                }
            };
            auto validateAccountAssets = [&](const TestAccount& account,
                                                int assetIndex, int difference,
                                                int feeCount) {
                for (size_t i = 0; i < pathSize; i++)
                {
                    validateAccountAsset(account, i,
                                            (assetIndex == i) ? difference : 0,
                                            feeCount);
                }
            };
            auto validateOffer = [offerAmount](const TestAccount& account,
                                                uint64_t offerId,
                                                int difference) {
                auto offer = account.loadOffer(offerId);
                auto offerEntry = offer->getOffer();
                REQUIRE(offerEntry.amount == offerAmount + difference);
            };

            auto source = setupAccount("S");
            auto destination = setupAccount("D");

            auto validateSource = [&](int difference) {
                validateAccountAssets(source, 0, difference, 3);
            };
            auto validateDestination = [&](int difference) {
                validateAccountAssets(destination, 0, difference, 2);
            };

            for (size_t i = 0; i < pathSize;
                    i++) // create account for each known asset
            {
                accounts.emplace_back(
                    setupAccount(std::string{"C"} + std::to_string(i)));
                validateAccountAssets(accounts[i], 0, 0,
                                        2); // 2x change trust called
            }

            auto testPath = [&](const std::string& name, const Price& price,
                                int maxMultipler, bool overSendMax) {
                SECTION(name)
                {
                    auto offers = std::deque<uint64_t>{};
                    for (size_t i = 0; i < pathSize; i++)
                    {
                        offers.push_back(accounts[i].manageOffer(
                            0, assets[i], assets[(i + 2) % pathSize], price,
                            offerAmount));
                        validateOffer(accounts[i], offers[i], 0);
                    }

                    for (size_t i = 0; i < pathSize; i++)
                    {
                        auto path =
                            std::vector<Asset>{assets[1], assets[2]};
                        SECTION(std::string{"send with path ("} +
                                assetPathToString(assets) + ")")
                        {
                            auto destinationMultiplier =
                                overSendMax ? 0 : 1;
                            auto sellerMultipler =
                                overSendMax ? Price{0, 1} : Price{1, 1};
                            auto buyerMultipler = sellerMultipler * price;

                            if (overSendMax)
                                REQUIRE_THROWS_AS(
                                    source.pay(destination, assets[0],
                                                maxMultipler * paymentAmount,
                                                assets[0], paymentAmount,
                                                path),
                                    ex_PATH_PAYMENT_OVER_SENDMAX);
                            else
                                source.pay(destination, assets[0],
                                            maxMultipler * paymentAmount,
                                            assets[0], paymentAmount, path);

                            for (size_t j = 0; j < pathSize; j++)
                            {
                                auto index = (pathSize - j) %
                                                pathSize; // it is done from
                                                        // end of path to
                                                        // begin of path
                                validateAccountAsset(accounts[index], index,
                                                        -paymentAmount *
                                                            sellerMultipler,
                                                        3); // sold asset
                                validateOffer(
                                    accounts[index], offers[index],
                                    -paymentAmount *
                                        sellerMultipler); // sold asset
                                validateAccountAsset(
                                    accounts[index], (index + 2) % pathSize,
                                    paymentAmount * buyerMultipler,
                                    3); // bought asset
                                validateAccountAsset(accounts[index],
                                                        (index + 1) % pathSize,
                                                        0, 3); // ignored asset
                                sellerMultipler = sellerMultipler * price;
                                buyerMultipler = buyerMultipler * price;
                            }

                            validateSource(-paymentAmount *
                                            sellerMultipler);
                            validateDestination(paymentAmount *
                                                destinationMultiplier);
                        }

                        // next cycle variant
                        rotateRight(assets);
                        rotateRight(accounts);
                        rotateRight(offers);
                    }
                }
            };

            // cycle with every asset on path costing half as much as
            // previous - 8 times gain
            testPath("arbitrage", Price(1, 2), 1, false);
            // cycle with every asset on path costing twice as much as
            // previous - 8 times loss - unacceptable
            testPath("anti-arbitrage", Price(2, 1), 1, true);
            // cycle with every asset on path costing twice as much as
            // previous - 8 times loss - acceptable (but not wise to do)
            testPath("anti-arbitrage with big sendmax", Price(2, 1), 8,
                        false);
        }
    });

    auto amount = 1;
    SECTION("fee less than base reserve")
    {
        SECTION("account has only base reserve + amount")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount);
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 1);
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee - one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee - 1);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_PAYMENT_UNDERFUNDED);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + one operation fee + one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee + 1);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees - two stroops")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee - 2);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees - one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee - 1);
            for_all_versions(app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee);
            for_all_versions(app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }
    }

    SECTION("fee equal to base reserve")
    {
        auto cfg = getTestConfig(1);
        cfg.DESIRED_BASE_FEE = 100000000;

        VirtualClock clock;
        ApplicationEditableVersion app(clock, cfg);
        app.start();

        // set up world
        auto root = TestAccount::createRoot(app);
        auto txfee = app.getLedgerManager().getTxFee();

        SECTION("account has only base reserve + amount")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount);
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 1);
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee - one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee - 1);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_PAYMENT_UNDERFUNDED);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + one operation fee + one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee + 1);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees - two stroops")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee - 2);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees - one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee - 1);
            for_all_versions(app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee);
            for_all_versions(app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }
    }

    SECTION("fee bigger than base reserve")
    {
        auto cfg = getTestConfig(1);
        cfg.DESIRED_BASE_FEE = 200000000;

        VirtualClock clock;
        ApplicationEditableVersion app(clock, cfg);
        app.start();

        // set up world
        auto root = TestAccount::createRoot(app);
        auto txfee = app.getLedgerManager().getTxFee();

        SECTION("account has only base reserve + amount")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount);
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 1);
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee - one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee - 1);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_PAYMENT_UNDERFUNDED);
            });
        }

        SECTION("account has only base reserve + amount + one operation fee")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + one operation fee + one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + txfee + 1);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees - two stroops")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee - 2);
            for_versions_to(8, app, [&]{
                REQUIRE_THROWS_AS(payFrom.pay(root, 1), ex_txINSUFFICIENT_BALANCE);
            });
            for_versions_from(9, app, [&]{
                REQUIRE_NOTHROW(payFrom.pay(root, 1));
            });
        }

        SECTION("account has only base reserve + amount + two operation fees - one stroop")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee - 1);
            for_all_versions(app, [&]{
                payFrom.pay(root, 1);
            });
        }

        SECTION("account has only base reserve + amount + two operation fees")
        {
            auto payFrom = root.create("pay-from", app.getLedgerManager().getMinBalance(0) + amount + 2 * txfee);
            for_all_versions(app, [&]{
                payFrom.pay(root, 1);
            });
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
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    auto root = TestAccount::createRoot(*app);
    SecretKey a1 = getAccount("A");
    int64_t txfee = app->getLedgerManager().getTxFee();
    const int64_t paymentAmount =
        app->getLedgerManager().getMinBalance(1) + txfee * 10;

    {
        auto ctx = app->getDatabase().captureAndLogSQL("createAccount");
        auto a1 = root.create("A", paymentAmount);
    }
}
