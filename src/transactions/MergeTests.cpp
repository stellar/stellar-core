// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/MergeOpFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include <lib/catch.hpp>

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("merge", "[tx][merge]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    ApplicationEditableVersion app(clock, cfg);
    app.start();

    // set up world
    // set up world
    auto root = TestAccount::createRoot(app);

    const int64_t assetMultiplier = 1000000;

    int64_t trustLineBalance = 100000 * assetMultiplier;
    int64_t trustLineLimit = trustLineBalance * 10;

    int64_t txfee = app.getLedgerManager().getTxFee();

    const int64_t minBalance =
        app.getLedgerManager().getMinBalance(5) + 20 * txfee;

    auto a1 = root.create("A", 2 * minBalance);

    SECTION("merge into self")
    {
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(a1.merge(a1), ex_ACCOUNT_MERGE_MALFORMED);
        });
    }

    SECTION("merge into non existent account")
    {
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(a1.merge(getAccount("B").getPublicKey()),
                            ex_ACCOUNT_MERGE_NO_ACCOUNT);
        });
    }

    auto b1 = root.create("B", minBalance);
    auto gateway = root.create("gate", minBalance);

    SECTION("with create")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto createBalance = app.getLedgerManager().getMinBalance(1);
        auto txFrame =
            a1.tx({a1.op(accountMerge(b1)),
                   b1.op(createAccount(a1.getPublicKey(), createBalance)),
                   a1.op(accountMerge(b1))});
        txFrame->addSignature(b1.getSecretKey());

        for_versions_to(5, app, [&]{
            applyCheck(txFrame, app);

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[2]);

            REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(b1.getBalance() ==
                    2 * a1Balance + b1Balance - createBalance -
                        2 * txFrame->getFee());
            REQUIRE(!loadAccount(a1, app, false));
        });

        for_versions_from(6, app, [&]{
            applyCheck(txFrame, app);

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[2]);

            REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(b1.getBalance() ==
                    a1Balance + b1Balance - txFrame->getFee());
            REQUIRE(!loadAccount(a1, app, false));
        });
    }

    SECTION("merge, create, merge back")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto a1SeqNum = a1.loadSequenceNumber();
        auto createBalance = app.getLedgerManager().getMinBalance(1);
        auto txFrame =
            a1.tx({a1.op(accountMerge(b1)),
                   b1.op(createAccount(a1.getPublicKey(), createBalance)),
                   b1.op(accountMerge(a1))});
        txFrame->addSignature(b1.getSecretKey());

        for_all_versions(app, [&]{
            applyCheck(txFrame, app);

            auto mergeResult = txFrame->getResult().result.results()[2].tr().accountMergeResult();
            REQUIRE(mergeResult.code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(mergeResult.sourceAccountBalance() == a1Balance + b1Balance - createBalance - txFrame->getFee());
            REQUIRE(a1.getBalance() ==
                    a1Balance + b1Balance - txFrame->getFee());
            REQUIRE(!loadAccount(b1, app, false));
            REQUIRE(a1SeqNum == a1.loadSequenceNumber());
        });
    }

    SECTION("merge, create, merge into the same")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto a1SeqNum = a1.loadSequenceNumber();
        auto b1SeqNum = b1.loadSequenceNumber();
        auto createBalance = app.getLedgerManager().getMinBalance(1);
        auto tx =
            a1.tx({accountMerge(b1),
                   createAccount(b1, createBalance),
                   accountMerge(b1)});

        for_versions_to(4, app, [&]{
            REQUIRE(!applyCheck(tx, app));

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFee());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().sourceAccountBalance() == a1Balance - tx->getFee());
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[1].tr().createAccountResult().code() == CREATE_ACCOUNT_ALREADY_EXIST);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[2].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[2].tr().accountMergeResult().sourceAccountBalance() == a1Balance - tx->getFee());
        });

        for_versions(5, 7, app, [&]{
            REQUIRE(!applyCheck(tx, app));

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFee());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().sourceAccountBalance() == a1Balance - tx->getFee());
            REQUIRE(tx->getResult().result.results()[1].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[1].tr().type() == CREATE_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[1].tr().createAccountResult().code() == CREATE_ACCOUNT_ALREADY_EXIST);
            REQUIRE(tx->getResult().result.results()[2].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[2].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[2].tr().accountMergeResult().code() == ACCOUNT_MERGE_NO_ACCOUNT);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(tx, app));

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFee());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().sourceAccountBalance() == a1Balance - tx->getFee());
            REQUIRE(tx->getResult().result.results()[1].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
        });
    }

    SECTION("merge, create different, merge into the same")
    {
        auto c1 = getAccount("c1");
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto a1SeqNum = a1.loadSequenceNumber();
        auto b1SeqNum = b1.loadSequenceNumber();
        auto createBalance = app.getLedgerManager().getMinBalance(1);
        auto tx =
            a1.tx({accountMerge(b1),
                   createAccount(c1.getPublicKey(), createBalance),
                   accountMerge(b1)});

        for_versions_to(7, app, [&]{
            REQUIRE(!applyCheck(tx, app));

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(!loadAccount(c1.getPublicKey(), app, false));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFee());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);

            REQUIRE(tx->getResult().result.code() == txINTERNAL_ERROR);
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(tx, app));

            REQUIRE(loadAccount(a1, app));
            REQUIRE(loadAccount(b1, app));
            REQUIRE(!loadAccount(c1.getPublicKey(), app, false));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFee());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);

            REQUIRE(tx->getResult().result.results()[0].code() == opINNER);
            REQUIRE(tx->getResult().result.results()[0].tr().type() == ACCOUNT_MERGE);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(tx->getResult().result.results()[0].tr().accountMergeResult().sourceAccountBalance() == a1Balance - tx->getFee());
            REQUIRE(tx->getResult().result.results()[1].code() == opNO_ACCOUNT);
            REQUIRE(tx->getResult().result.results()[2].code() == opNO_ACCOUNT);
        });
    }

    SECTION("merge account twice")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();

        auto txFrame =
            a1.tx({accountMerge(b1), accountMerge(b1)});

        for_versions_to(4, app, [&]{
            REQUIRE(applyCheck(txFrame, app));

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[1]);

            auto a1BalanceAfterFee = a1Balance - txFrame->getFee();
            REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(b1Balance + a1BalanceAfterFee + a1BalanceAfterFee ==
                    b1.getBalance());
            REQUIRE(!loadAccount(a1, app, false));
        });

        for_versions(5, 7, app, [&]{
            REQUIRE(!applyCheck(txFrame, app));

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[1]);

            REQUIRE(result == ACCOUNT_MERGE_NO_ACCOUNT);
            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFee()) ==
                    a1.getBalance());
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(txFrame, app));
            REQUIRE(txFrame->getResult().result.results()[1].code() == opNO_ACCOUNT);
        });
    }

    SECTION("merge account twice to non existing account")
    {
        auto a1Balance = a1.getBalance();

        auto txFrame =
            a1.tx({accountMerge(getAccount("non-existing").getPublicKey()),
                   accountMerge(getAccount("non-existing").getPublicKey())});

        for_all_versions(app, [&]{
            applyCheck(txFrame, app);

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[1]);

            auto a1BalanceAfterFee = a1Balance - txFrame->getFee();
            REQUIRE(result == ACCOUNT_MERGE_NO_ACCOUNT);
            REQUIRE(a1Balance - txFrame->getFee() == a1.getBalance());
            REQUIRE(loadAccount(a1, app, false));
        });
    }

    SECTION("Account has static auth flag set")
    {
        for_all_versions(app, [&]{
            uint32 flags = AUTH_IMMUTABLE_FLAG;
            a1.setOptions(nullptr, &flags, nullptr, nullptr, nullptr, nullptr);

            REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_IMMUTABLE_SET);
        });
    }

    SECTION("With sub entries")
    {
        Asset usdCur = makeAsset(gateway, "USD");
        a1.changeTrust(usdCur, trustLineLimit);

        SECTION("account has trust line")
        {
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }
        SECTION("account has offer")
        {
            for_all_versions(app, [&]{
                gateway.pay(a1, usdCur, trustLineBalance);
                Asset xlmCur;
                xlmCur.type(AssetType::ASSET_TYPE_NATIVE);

                const Price somePrice(3, 2);
                for (int i = 0; i < 4; i++)
                {
                    a1.manageOffer(0, xlmCur, usdCur, somePrice,
                                    100 * assetMultiplier);
                }
                // empty out balance
                a1.pay(gateway, usdCur, trustLineBalance);
                // delete the trust line
                a1.changeTrust(usdCur, 0);

                REQUIRE_THROWS_AS(a1.merge(b1),
                                    ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }

        SECTION("account has data")
        {
            for_versions_from({2, 4}, app, [&]{
                // delete the trust line
                a1.changeTrust(usdCur, 0);

                DataValue value;
                value.resize(20);
                for (auto n = 0u; n < 20; n++)
                {
                    value[n] = (unsigned char)n;
                }

                std::string t1("test");

                a1.manageData(t1, &value);
                REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }
    }

    SECTION("success")
    {
        SECTION("success - basic")
        {
            for_all_versions(app, [&]{
                a1.merge(b1);
                REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                                app.getDatabase()));
            });
        }
        SECTION("success, invalidates dependent tx")
        {
            for_all_versions(app, [&]{
                auto tx1 = a1.tx({accountMerge(b1)});
                auto tx2 = a1.tx({payment(root, 100)});
                auto a1Balance = a1.getBalance();
                auto b1Balance = b1.getBalance();
                auto r = closeLedgerOn(app, 2, 1, 1, 2015, {tx1, tx2});
                checkTx(0, r, txSUCCESS);
                checkTx(1, r, txNO_ACCOUNT);

                REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                                app.getDatabase()));

                int64 expectedB1Balance =
                    a1Balance + b1Balance - 2 * app.getLedgerManager().getTxFee();
                REQUIRE(expectedB1Balance == b1.getBalance());
            });
        }
    }

    SECTION("account has only base reserve")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0));
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one stroop")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0) + 1);
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one operation fee - one stroop")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0) + txfee - 1);
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one operation fee")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0) + txfee);
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one operation fee + one stroop")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0) + txfee + 1);
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + two operation fees - one stroop")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0) + 2 * txfee - 1);
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + two operation fees")
    {
        auto mergeFrom = root.create("merge-from", app.getLedgerManager().getMinBalance(0) + 2 * txfee);
        for_all_versions(app, [&]{
            mergeFrom.merge(root);
        });
    }
}
