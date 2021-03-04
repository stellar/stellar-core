// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("merge", "[tx][merge]")
{
    Config cfg(getTestConfig());

    // Do our setup in version 1 so that for_all_versions below does not
    // try to downgrade us from >1 to 1.
    cfg.USE_CONFIG_FOR_GENESIS = false;

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    // set up world
    auto root = TestAccount::createRoot(*app);

    int64_t trustLineBalance = 100000;
    int64_t trustLineLimit = trustLineBalance * 10;

    auto txfee = app->getLedgerManager().getLastTxFee();

    const int64_t minBalance =
        app->getLedgerManager().getLastMinBalance(5) + 20 * txfee;

    auto a1 = root.create("A", 2 * minBalance);
    auto b1 = root.create("B", minBalance);
    auto gateway = root.create("gate", minBalance);

    // close ledger to allow a1, b1 and gateway to be merged in the next ledger
    closeLedgerOn(*app, 2, 1, 1, 2016);

    SECTION("merge into self")
    {
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(a1.merge(a1), ex_ACCOUNT_MERGE_MALFORMED);
        });
    }

    SECTION("merge into non existent account")
    {
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(a1.merge(getAccount("C").getPublicKey()),
                              ex_ACCOUNT_MERGE_NO_ACCOUNT);
        });
    }

    SECTION("with create")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto createBalance = app->getLedgerManager().getLastMinBalance(1);

        for_versions_to(5, *app, [&] {
            auto txFrame =
                a1.tx({a1.op(accountMerge(b1)),
                       b1.op(createAccount(a1.getPublicKey(), createBalance)),
                       a1.op(accountMerge(b1))});
            txFrame->addSignature(b1.getSecretKey());

            auto applyResult = expectedResult(
                txfee * 3, 3, txSUCCESS,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()},
                 CREATE_ACCOUNT_SUCCESS,
                 {ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()}});
            validateTxResults(txFrame, *app, {txfee * 3, txSUCCESS},
                              applyResult);

            REQUIRE(b1.getBalance() == 2 * a1Balance + b1Balance -
                                           createBalance -
                                           2 * txFrame->getFeeBid());
            REQUIRE(!doesAccountExist(*app, a1));
        });

        for_versions(6, 9, *app, [&] {
            auto txFrame =
                a1.tx({a1.op(accountMerge(b1)),
                       b1.op(createAccount(a1.getPublicKey(), createBalance)),
                       a1.op(accountMerge(b1))});
            txFrame->addSignature(b1.getSecretKey());

            auto applyResult = expectedResult(
                txfee * 3, 3, txSUCCESS,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()},
                 CREATE_ACCOUNT_SUCCESS,
                 {ACCOUNT_MERGE_SUCCESS, createBalance}});
            validateTxResults(txFrame, *app, {txfee * 3, txSUCCESS},
                              applyResult);

            REQUIRE(b1.getBalance() ==
                    a1Balance + b1Balance - txFrame->getFeeBid());
            REQUIRE(!doesAccountExist(*app, a1));
        });

        for_versions_from(10, *app, [&]() {
            auto txFrame =
                a1.tx({a1.op(accountMerge(b1)),
                       b1.op(createAccount(a1.getPublicKey(), createBalance)),
                       a1.op(accountMerge(b1))});
            txFrame->addSignature(b1.getSecretKey());

            // can't merge an account that just got created
            auto applyResult = expectedResult(
                txfee * 3, 3, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()},
                 CREATE_ACCOUNT_SUCCESS,
                 ACCOUNT_MERGE_SEQNUM_TOO_FAR});
            validateTxResults(txFrame, *app, {txfee * 3, txSUCCESS},
                              applyResult);
        });
    }

    SECTION("merge, create, merge back")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto createBalance = app->getLedgerManager().getLastMinBalance(1);

        for_all_versions(*app, [&] {
            auto txFrame =
                a1.tx({a1.op(accountMerge(b1)),
                       b1.op(createAccount(a1.getPublicKey(), createBalance)),
                       b1.op(accountMerge(a1))});
            txFrame->addSignature(b1.getSecretKey());

            // a1 gets re-created so we disable sequence number checks
            applyCheck(txFrame, *app, false);

            auto mergeResult = txFrame->getResult()
                                   .result.results()[2]
                                   .tr()
                                   .accountMergeResult();
            REQUIRE(mergeResult.code() == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(mergeResult.sourceAccountBalance() ==
                    a1Balance + b1Balance - createBalance -
                        txFrame->getFeeBid());
            REQUIRE(a1.getBalance() ==
                    a1Balance + b1Balance - txFrame->getFeeBid());
            REQUIRE(!doesAccountExist(*app, b1));
            // a1 gets recreated with a sequence number based on the current
            // ledger
            REQUIRE(a1.loadSequenceNumber() == 0x300000000ull);
        });
    }

    SECTION("merge, create, merge into the same")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto a1SeqNum = a1.loadSequenceNumber();
        auto b1SeqNum = b1.loadSequenceNumber();
        auto createBalance = app->getLedgerManager().getLastMinBalance(1);

        for_versions_to(4, *app, [&] {
            auto tx = a1.tx({accountMerge(b1), createAccount(b1, createBalance),
                             accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 3, 3, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - tx->getFeeBid()},
                 CREATE_ACCOUNT_ALREADY_EXIST,
                 {ACCOUNT_MERGE_SUCCESS, a1Balance - tx->getFeeBid()}});
            validateTxResults(tx, *app, {txfee * 3, txSUCCESS}, applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(doesAccountExist(*app, b1));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFeeBid());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);
        });

        for_versions(5, 7, *app, [&] {
            auto tx = a1.tx({accountMerge(b1), createAccount(b1, createBalance),
                             accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 3, 3, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - tx->getFeeBid()},
                 CREATE_ACCOUNT_ALREADY_EXIST,
                 ACCOUNT_MERGE_NO_ACCOUNT});
            validateTxResults(tx, *app, {txfee * 3, txSUCCESS}, applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(doesAccountExist(*app, b1));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFeeBid());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);
        });

        for_versions_from(8, *app, [&] {
            auto tx = a1.tx({accountMerge(b1), createAccount(b1, createBalance),
                             accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 3, 3, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - tx->getFeeBid()},
                 opNO_ACCOUNT,
                 opNO_ACCOUNT});
            validateTxResults(tx, *app, {txfee * 3, txSUCCESS}, applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(doesAccountExist(*app, b1));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFeeBid());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);
        });
    }

    SECTION("merge, create different, merge into the same")
    {
        auto c1 = getAccount("c1");
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();
        auto a1SeqNum = a1.loadSequenceNumber();
        auto b1SeqNum = b1.loadSequenceNumber();
        auto createBalance = app->getLedgerManager().getLastMinBalance(1);

        for_versions_to(7, *app, [&] {
            auto tx = a1.tx({accountMerge(b1),
                             createAccount(c1.getPublicKey(), createBalance),
                             accountMerge(b1)});

            auto applyResult = expectedResult(txfee * 3, 3, txINTERNAL_ERROR);
            validateTxResults(tx, *app, {txfee * 3, txSUCCESS}, applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(doesAccountExist(*app, b1));
            REQUIRE(!doesAccountExist(*app, c1.getPublicKey()));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFeeBid());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);
        });

        for_versions_from(8, *app, [&] {
            auto tx = a1.tx({accountMerge(b1),
                             createAccount(c1.getPublicKey(), createBalance),
                             accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 3, 3, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - tx->getFeeBid()},
                 opNO_ACCOUNT,
                 opNO_ACCOUNT});
            validateTxResults(tx, *app, {txfee * 3, txSUCCESS}, applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(doesAccountExist(*app, b1));
            REQUIRE(!doesAccountExist(*app, c1.getPublicKey()));
            REQUIRE(a1.getBalance() == a1Balance - tx->getFeeBid());
            REQUIRE(b1.getBalance() == b1Balance);
            REQUIRE(a1.loadSequenceNumber() == a1SeqNum + 1);
            REQUIRE(b1.loadSequenceNumber() == b1SeqNum);
        });
    }

    SECTION("merge account twice")
    {
        auto a1Balance = a1.getBalance();
        auto b1Balance = b1.getBalance();

        for_versions_to(4, *app, [&] {
            auto txFrame = a1.tx({accountMerge(b1), accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 2, 2, txSUCCESS,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()},
                 {ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()}});
            validateTxResults(txFrame, *app, {txfee * 2, txSUCCESS},
                              applyResult);

            auto a1BalanceAfterFee = a1Balance - txFrame->getFeeBid();
            REQUIRE(b1Balance + a1BalanceAfterFee + a1BalanceAfterFee ==
                    b1.getBalance());
            REQUIRE(!doesAccountExist(*app, a1));
        });

        for_versions(5, 7, *app, [&] {
            auto txFrame = a1.tx({accountMerge(b1), accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 2, 2, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()},
                 ACCOUNT_MERGE_NO_ACCOUNT});
            validateTxResults(txFrame, *app, {txfee * 2, txSUCCESS},
                              applyResult);

            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFeeBid()) == a1.getBalance());
        });

        for_versions_from(8, *app, [&] {
            auto txFrame = a1.tx({accountMerge(b1), accountMerge(b1)});

            auto applyResult = expectedResult(
                txfee * 2, 2, txFAILED,
                {{ACCOUNT_MERGE_SUCCESS, a1Balance - txFrame->getFeeBid()},
                 opNO_ACCOUNT});
            validateTxResults(txFrame, *app, {txfee * 2, txSUCCESS},
                              applyResult);

            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFeeBid()) == a1.getBalance());
        });
    }

    SECTION("merge account twice to non existing account")
    {
        auto a1Balance = a1.getBalance();

        for_all_versions(*app, [&] {
            auto txFrame = a1.tx(
                {accountMerge(getAccount("non-existing").getPublicKey()),
                 accountMerge(getAccount("non-existing").getPublicKey())});

            applyCheck(txFrame, *app);

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[1]);

            auto a1BalanceAfterFee = a1Balance - txFrame->getFeeBid();
            REQUIRE(result == ACCOUNT_MERGE_NO_ACCOUNT);
            REQUIRE(a1BalanceAfterFee == a1.getBalance());
            REQUIRE(doesAccountExist(*app, a1));
        });
    }

    SECTION("create, merge, create")
    {
        auto c = getAccount("C");
        auto d = getAccount("D");

        REQUIRE(doesAccountExist(*app, a1));
        REQUIRE(!doesAccountExist(*app, c.getPublicKey()));
        REQUIRE(!doesAccountExist(*app, d.getPublicKey()));
        auto a1Balance = a1.getBalance();
        auto createBalance = app->getLedgerManager().getLastMinBalance(0);

        for_versions_to(7, *app, [&] {
            auto txFrame =
                a1.tx({createAccount(c.getPublicKey(), createBalance),
                       accountMerge(c.getPublicKey()),
                       createAccount(d.getPublicKey(), createBalance)});

            auto applyResult = expectedResult(txfee * 3, 3, txINTERNAL_ERROR);
            validateTxResults(txFrame, *app, {txfee * 3, txSUCCESS},
                              applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(!doesAccountExist(*app, c.getPublicKey()));
            REQUIRE(!doesAccountExist(*app, d.getPublicKey()));
            REQUIRE(a1Balance == a1.getBalance() + txFrame->getFeeBid());
        });

        for_versions_from(8, *app, [&] {
            auto txFrame =
                a1.tx({createAccount(c.getPublicKey(), createBalance),
                       accountMerge(c.getPublicKey()),
                       createAccount(d.getPublicKey(), createBalance)});

            auto applyResult = expectedResult(
                txfee * 3, 3, txFAILED,
                {CREATE_ACCOUNT_SUCCESS,
                 {ACCOUNT_MERGE_SUCCESS,
                  a1Balance - createBalance - txFrame->getFeeBid()},
                 opNO_ACCOUNT});
            validateTxResults(txFrame, *app, {txfee * 3, txSUCCESS},
                              applyResult);

            REQUIRE(doesAccountExist(*app, a1));
            REQUIRE(!doesAccountExist(*app, c.getPublicKey()));
            REQUIRE(!doesAccountExist(*app, d.getPublicKey()));
            REQUIRE(txFrame->getResultCode() == txFAILED);

            REQUIRE(a1Balance == a1.getBalance() + txFrame->getFeeBid());
        });
    }

    SECTION("Account has static auth flag set")
    {
        for_all_versions(*app, [&] {
            a1.setOptions(setFlags(AUTH_IMMUTABLE_FLAG));

            REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_IMMUTABLE_SET);
        });
    }

    SECTION("With sub entries")
    {
        SECTION("with trustline")
        {
            Asset usd = makeAsset(gateway, "USD");
            a1.changeTrust(usd, trustLineLimit);

            SECTION("account has trust line")
            {
                for_all_versions(*app, [&] {
                    REQUIRE_THROWS_AS(a1.merge(b1),
                                      ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
                });
            }
            SECTION("account has offer")
            {
                for_all_versions(*app, [&] {
                    gateway.pay(a1, usd, trustLineBalance);
                    auto xlm = makeNativeAsset();
                    auto curIssued = a1.asset("CUR1");

                    const Price somePrice(3, 2);
                    for (int i = 0; i < 4; i++)
                    {
                        a1.manageOffer(0, xlm, curIssued, somePrice, 100);
                    }
                    REQUIRE_THROWS_AS(a1.merge(b1),
                                      ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
                });
            }
        }

        SECTION("account has data")
        {
            for_versions_from({2, 4}, *app, [&] {
                DataValue value;
                value.resize(20);
                for (int n = 0; n < 20; n++)
                {
                    value[n] = (unsigned char)n;
                }

                std::string t1("test");

                a1.manageData(t1, &value);
                REQUIRE_THROWS_AS(a1.merge(b1),
                                  ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }
        SECTION("account has signer")
        {
            for_all_versions(*app, [&]() {
                a1.setOptions(setSigner(makeSigner(gateway, 5)));
                a1.merge(b1);
            });
        }
    }

    SECTION("success")
    {
        SECTION("success - basic")
        {
            for_all_versions(*app, [&] {
                a1.merge(b1);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!stellar::loadAccount(ltx, a1.getPublicKey()));
                }
            });
        }
        SECTION("success, invalidates dependent tx")
        {
            for_all_versions(*app, [&] {
                auto tx1 = a1.tx({accountMerge(b1)});
                auto tx2 = a1.tx({payment(root, 100)});
                auto a1Balance = a1.getBalance();
                auto b1Balance = b1.getBalance();
                auto r = closeLedgerOn(*app, 3, 1, 1, 2017, {tx1, tx2},
                                       /* strictOrder */ true);
                checkTx(0, r, txSUCCESS);
                checkTx(1, r, txNO_ACCOUNT);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!stellar::loadAccount(ltx, a1.getPublicKey()));
                }

                int64 expectedB1Balance =
                    a1Balance + b1Balance -
                    2 * app->getLedgerManager().getLastTxFee();
                REQUIRE(expectedB1Balance == b1.getBalance());
            });
        }
    }

    SECTION("account has only base reserve")
    {
        auto mergeFrom = root.create(
            "merge-from", app->getLedgerManager().getLastMinBalance(0));
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one stroop")
    {
        auto mergeFrom = root.create(
            "merge-from", app->getLedgerManager().getLastMinBalance(0) + 1);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one operation fee - one stroop")
    {
        auto mergeFrom = root.create(
            "merge-from",
            app->getLedgerManager().getLastMinBalance(0) + txfee - 1);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
    }

    SECTION("account has only base reserve + one operation fee")
    {
        auto mergeFrom = root.create(
            "merge-from", app->getLedgerManager().getLastMinBalance(0) + txfee);
        closeLedgerOn(*app, 3, 1, 1, 2017);
        for_versions_to(8, *app, [&] {
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
        for_versions_from(9, *app,
                          [&] { REQUIRE_NOTHROW(mergeFrom.merge(root)); });
    }

    SECTION("account has only base reserve + one operation fee + one stroop")
    {
        auto mergeFrom = root.create(
            "merge-from",
            app->getLedgerManager().getLastMinBalance(0) + txfee + 1);
        closeLedgerOn(*app, 3, 1, 1, 2017);
        for_versions_to(8, *app, [&] {
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
        for_versions_from(9, *app,
                          [&] { REQUIRE_NOTHROW(mergeFrom.merge(root)); });
    }

    SECTION("account has only base reserve + two operation fees - one stroop")
    {
        auto mergeFrom = root.create(
            "merge-from",
            app->getLedgerManager().getLastMinBalance(0) + 2 * txfee - 1);
        closeLedgerOn(*app, 3, 1, 1, 2017);
        for_versions_to(8, *app, [&] {
            REQUIRE_THROWS_AS(mergeFrom.merge(root), ex_txINSUFFICIENT_BALANCE);
        });
        for_versions_from(9, *app,
                          [&] { REQUIRE_NOTHROW(mergeFrom.merge(root)); });
    }

    SECTION("account has only base reserve + two operation fees")
    {
        auto mergeFrom = root.create(
            "merge-from",
            app->getLedgerManager().getLastMinBalance(0) + 2 * txfee);
        closeLedgerOn(*app, 3, 1, 1, 2017);
        for_all_versions(*app, [&] { mergeFrom.merge(root); });
    }

    SECTION("merge too far")
    {
        for_versions_from(10, *app, [&]() {
            SequenceNumber curStartSeqNum;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ++ltx.loadHeader().current().ledgerSeq;
                curStartSeqNum = getStartingSequenceNumber(ltx.loadHeader());
            }
            auto maxSeqNum = curStartSeqNum - 1;

            auto txFrame = root.tx({a1.op(accountMerge(b1))});
            txFrame->addSignature(a1.getSecretKey());

            SECTION("at max = success")
            {
                a1.bumpSequence(maxSeqNum);
                REQUIRE(a1.loadSequenceNumber() == maxSeqNum);
                REQUIRE(applyCheck(txFrame, *app));
            }
            SECTION("passed max = failure")
            {
                maxSeqNum++;
                a1.bumpSequence(maxSeqNum);
                REQUIRE(a1.loadSequenceNumber() == maxSeqNum);

                REQUIRE(!applyCheck(txFrame, *app));
                REQUIRE(txFrame->getResult()
                            .result.results()[0]
                            .tr()
                            .accountMergeResult()
                            .code() == ACCOUNT_MERGE_SEQNUM_TOO_FAR);
            }
        });
    }

    SECTION("destination with native buying liabilities")
    {
        auto& lm = app->getLedgerManager();
        auto minBal = lm.getLastMinBalance(1);
        auto acc1 = root.create("acc1", minBal + txfee);
        auto acc2 = root.create("acc2", minBal + txfee + 1);

        auto const native = makeNativeAsset();
        auto cur1 = acc1.asset("CUR1");

        for_versions_to(9, *app, [&] {
            TestMarket market(*app);
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(
                    acc1, {cur1, native, Price{1, 1}, INT64_MAX - 2 * minBal});
            });
            acc2.merge(acc1);
            REQUIRE(acc1.getBalance() == 2 * minBal + 1);
        });
        for_versions_from(10, *app, [&] {
            TestMarket market(*app);
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(
                    acc1, {cur1, native, Price{1, 1}, INT64_MAX - 2 * minBal});
            });
            closeLedgerOn(*app, 3, 1, 1, 2017);
            REQUIRE_THROWS_AS(acc2.merge(acc1), ex_ACCOUNT_MERGE_DEST_FULL);
            root.pay(acc2, txfee - 1);
            acc2.merge(acc1);

            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto header = ltx.loadHeader();
                auto account = stellar::loadAccount(ltx, acc1.getPublicKey());
                auto const& ae = account.current().data.account();
                REQUIRE(ae.balance == 2 * minBal);
                REQUIRE(ae.balance + getBuyingLiabilities(header, account) ==
                        INT64_MAX);
            }
        });
    }

    SECTION("sponsorships")
    {
        auto sponsoringAcc = root.create("sponsoringAcc", minBalance);
        auto addSponsoredSigner =
            [&](TestAccount& dest, int leExt, AccountID const* sponsoringID,
                uint32_t numSubEntries, int aeExt, uint32_t numSponsoring,
                uint32_t numSponsored) {
                // add sponsored signer
                auto signer = makeSigner(getAccount("S1"), 1);
                auto tx = transactionFrameFromOps(
                    app->getNetworkID(), dest,
                    {sponsoringAcc.op(beginSponsoringFutureReserves(dest)),
                     dest.op(setOptions(setSigner(signer))),
                     dest.op(endSponsoringFutureReserves())},
                    {sponsoringAcc});

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, dest, signer.key, 2,
                                     &sponsoringAcc.getPublicKey());
                    checkSponsorship(ltx, sponsoringAcc, leExt, sponsoringID,
                                     numSubEntries, aeExt, numSponsoring,
                                     numSponsored);
                    ltx.commit();
                }
            };

        for_versions_from(14, *app, [&] {
            SECTION("with sponsored signers")
            {
                // add non-sponsored signer
                a1.setOptions(setSigner(makeSigner(gateway, 5)));
                addSponsoredSigner(a1, 0, nullptr, 0, 2, 1, 0);

                a1.merge(b1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                checkSponsorship(ltx, sponsoringAcc, 0, nullptr, 0, 2, 0, 0);
            }

            SECTION("with sponsored account")
            {
                auto key = getAccount("acc1");
                TestAccount acc1(*app, key);
                auto tx = transactionFrameFromOps(
                    app->getNetworkID(), sponsoringAcc,
                    {sponsoringAcc.op(beginSponsoringFutureReserves(acc1)),
                     sponsoringAcc.op(createAccount(acc1, txfee * 4)),
                     acc1.op(endSponsoringFutureReserves())},
                    {key});

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(tx->apply(*app, ltx, txm));

                    checkSponsorship(ltx, key.getPublicKey(), 1,
                                     &sponsoringAcc.getPublicKey(), 0, 2, 0, 2);
                    ltx.commit();
                }

                auto merge = [&](bool addSigner) {
                    if (addSigner)
                    {
                        addSponsoredSigner(
                            acc1, 0, &sponsoringAcc.getPublicKey(), 0, 2, 3, 0);
                    }

                    acc1.merge(b1);

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    checkSponsorship(ltx, sponsoringAcc.getPublicKey(), 0,
                                     nullptr, 0, 2, 0, 0);
                };

                SECTION("without sponsored signer")
                {
                    merge(false);
                }

                SECTION("with sponsored signer")
                {
                    merge(true);
                }
            }

            SECTION("is sponsor error")
            {
                // close ledger to increase ledger seq num so we don't hit
                // ACCOUNT_MERGE_SEQNUM_TOO_FAR
                closeLedgerOn(*app, 3, 1, 1, 2016);

                SECTION("is sponsoring future reserves")
                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {a1.op(beginSponsoringFutureReserves(b1)),
                         a1.op(accountMerge(b1)),
                         b1.op(endSponsoringFutureReserves())},
                        {b1});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMeta txm(2);
                    REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                    REQUIRE(!tx->apply(*app, ltx, txm));
                    REQUIRE(tx->getResult()
                                .result.results()[1]
                                .tr()
                                .accountMergeResult()
                                .code() == ACCOUNT_MERGE_IS_SPONSOR);
                }

                SECTION("is sponsoring reserve")
                {
                    auto cur1 = makeAsset(root, "CUR1");
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), a1,
                        {sponsoringAcc.op(beginSponsoringFutureReserves(a1)),
                         a1.op(changeTrust(cur1, 1000)),
                         a1.op(endSponsoringFutureReserves())},
                        {sponsoringAcc});

                    {
                        LedgerTxn ltx(app->getLedgerTxnRoot());
                        TransactionMeta txm(2);
                        REQUIRE(tx->checkValid(ltx, 0, 0, 0));
                        REQUIRE(tx->apply(*app, ltx, txm));

                        checkSponsorship(ltx, sponsoringAcc, 0, nullptr, 0, 2,
                                         1, 0);
                        ltx.commit();
                    }

                    REQUIRE_THROWS_AS(sponsoringAcc.merge(b1),
                                      ex_ACCOUNT_MERGE_IS_SPONSOR);

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    checkSponsorship(ltx, sponsoringAcc, 0, nullptr, 0, 2, 1,
                                     0);
                }
            }
        });
    }
}
