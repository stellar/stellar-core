// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/SignerKey.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "test/TestAccount.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionBridge.h"
#include "util/Timer.h"

#include <algorithm>
#include <lib/catch.hpp>
#include <xdrpp/printer.h>

using namespace stellar;
using namespace stellar::txbridge;
using namespace stellar::txtest;

/*
  Test how TxResults are created from valid and malformed transactions,
  depending on order of operations and position of bug. Validate lambda first
  tests if checkValid() returns proper value. Only TransactionResultCode and fee
  is checked. For apply operation whole TransactionResult is checked.

  TransactionResult from apply calls are stored in ledger after hashing as
  txSetResultHash so these cannot change between releases. TransactionResult
  from apply checkValid are not stored in ledger, so there may
  be minor differences between releases (main return code must remain the same,
  as it decides if transaction gets into valid tx set
  and then as scpValue in ledger or not).
*/

namespace
{

enum class Signed
{
    NOT_SIGNED,
    SIGNED,
    DOUBLE_SIGNED
};

enum class PaymentValidity
{
    VALID,
    MALFORMED,
    UNDERFUNDED
};
}

TEST_CASE("txresults", "[tx][txresults]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    const int64_t baseReserve = lm.getLastReserve();
    const int64_t baseFee = lm.getLastTxFee();
    const int64_t startAmount = baseReserve * 100;

    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        ltx.loadHeader().current().scpValue.closeTime = 10;
        ltx.commit();
    }

    auto getCloseTime = [&] {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        return ltx.loadHeader().current().scpValue.closeTime;
    };

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto a = root.create("a", startAmount);
    auto b = root.create("b", startAmount);
    auto c = root.create("c", startAmount);
    auto d = root.create("d", startAmount);
    auto e = root.create("e", startAmount);
    auto f = TestAccount{*app, getAccount("f")};
    auto g = root.create("g", lm.getLastMinBalance(0));

    SECTION("transaction errors")
    {
        SECTION("signed")
        {
            SECTION("missing operation")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({});
                    validateTxResults(tx, *app, {baseFee, txMISSING_OPERATION});
                });
            }

            SECTION("too early")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    setMinTime(tx, getCloseTime() + 1);
                    validateTxResults(tx, *app, {baseFee, txTOO_EARLY});
                });
            }

            SECTION("too late")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    setMaxTime(tx, getCloseTime() - 1);
                    validateTxResults(tx, *app, {baseFee, txTOO_LATE});
                });
            }

            SECTION("insufficent fee")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) - 1);
                    validateTxResults(tx, *app, {baseFee, txINSUFFICIENT_FEE});
                });
            }

            SECTION("no account")
            {
                for_all_versions(*app, [&] {
                    auto tx = f.tx({payment(root, 1)});
                    validateTxResults(tx, *app, {baseFee, txNO_ACCOUNT});
                });
            }

            SECTION("bad seq")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    setSeqNum(tx, tx->getSeqNum() + 1);
                    validateTxResults(tx, *app, {baseFee, txBAD_SEQ});
                });
            }

            SECTION("insufficent balance")
            {
                for_all_versions(*app, [&] {
                    auto tx = g.tx({payment(root, 1)});
                    validateTxResults(tx, *app,
                                      {baseFee, txINSUFFICIENT_BALANCE});
                });
            }
        }

        SECTION("not signed")
        {
            SECTION("missing operation")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({});
                    getSignatures(tx).clear();
                    validateTxResults(tx, *app, {baseFee, txMISSING_OPERATION});
                });
            }

            SECTION("too early")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    setMinTime(tx, getCloseTime() + 1);
                    validateTxResults(tx, *app, {baseFee, txTOO_EARLY});
                });
            }

            SECTION("too late")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    setMaxTime(tx, getCloseTime() - 1);
                    validateTxResults(tx, *app, {baseFee, txTOO_LATE});
                });
            }

            SECTION("insufficent fee")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) - 1);
                    validateTxResults(tx, *app, {baseFee, txINSUFFICIENT_FEE});
                });
            }

            SECTION("no account")
            {
                for_all_versions(*app, [&] {
                    auto tx = f.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    validateTxResults(tx, *app, {baseFee, txNO_ACCOUNT});
                });
            }

            SECTION("bad seq")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    setSeqNum(tx, tx->getSeqNum() + 1);
                    validateTxResults(tx, *app, {baseFee, txBAD_SEQ});
                });
            }

            SECTION("insufficent balance")
            {
                for_versions_to(6, *app, [&] {
                    auto tx = g.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    validateTxResults(tx, *app, {baseFee, txBAD_AUTH});
                });
                for_versions({7}, *app, [&] {
                    auto tx = g.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    validateTxResults(tx, *app,
                                      {baseFee, txINSUFFICIENT_BALANCE});
                });
                for_versions_from(8, *app, [&] {
                    auto tx = g.tx({payment(root, 1)});
                    getSignatures(tx).clear();
                    validateTxResults(tx, *app, {baseFee, txBAD_AUTH});
                });
            }
        }

        SECTION("double signed")
        {
            SECTION("missing operation")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({});
                    tx->addSignature(a);
                    validateTxResults(tx, *app, {baseFee, txMISSING_OPERATION});
                });
            }

            SECTION("too early")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    tx->addSignature(a);
                    setMinTime(tx, getCloseTime() + 1);
                    validateTxResults(tx, *app, {baseFee, txTOO_EARLY});
                });
            }

            SECTION("too late")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    tx->addSignature(a);
                    setMaxTime(tx, getCloseTime() - 1);
                    validateTxResults(tx, *app, {baseFee, txTOO_LATE});
                });
            }

            SECTION("insufficent fee")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    tx->addSignature(a);
                    setFee(tx, static_cast<uint32_t>(tx->getFeeBid()) - 1);
                    validateTxResults(tx, *app, {baseFee, txINSUFFICIENT_FEE});
                });
            }

            SECTION("no account")
            {
                for_all_versions(*app, [&] {
                    auto tx = f.tx({payment(root, 1)});
                    tx->addSignature(a);
                    validateTxResults(tx, *app, {baseFee, txNO_ACCOUNT});
                });
            }

            SECTION("bad seq")
            {
                for_all_versions(*app, [&] {
                    auto tx = a.tx({payment(root, 1)});
                    tx->addSignature(a);
                    setSeqNum(tx, tx->getSeqNum() + 1);
                    validateTxResults(tx, *app, {baseFee, txBAD_SEQ});
                });
            }

            SECTION("insufficent balance")
            {
                for_all_versions(*app, [&] {
                    auto tx = g.tx({payment(root, 1)});
                    tx->addSignature(a);
                    validateTxResults(tx, *app,
                                      {baseFee, txINSUFFICIENT_BALANCE});
                });
            }
        }
    }

    SECTION("merge account")
    {
        closeLedgerOn(*app, 2, 1, 1, 2016);
        SECTION("normal")
        {
            auto applyResult = expectedResult(
                baseFee * 2, 2, txSUCCESS,
                {PAYMENT_SUCCESS, {ACCOUNT_MERGE_SUCCESS, startAmount - 1200}});
            for_all_versions(*app, [&] {
                auto tx = a.tx({payment(b, 1000), accountMerge(root)});
                validateTxResults(tx, *app, {baseFee * 2, txSUCCESS},
                                  applyResult);
            });
        }

        SECTION("with operation after")
        {
            for_versions_to(7, *app, [&] {
                auto tx = a.tx(
                    {payment(b, 1000), accountMerge(root), payment(c, 1000)});
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(baseFee * 3, 3, txINTERNAL_ERROR));
            });
            for_versions_from(8, *app, [&] {
                auto tx = a.tx(
                    {payment(b, 1000), accountMerge(root), payment(c, 1000)});
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(baseFee * 3, 3, txFAILED,
                                   {PAYMENT_SUCCESS,
                                    {ACCOUNT_MERGE_SUCCESS,
                                     startAmount - tx->getFeeBid() - 1000},
                                    opNO_ACCOUNT}));
            });
        }
    }

    SECTION("create account")
    {
        SECTION("normal")
        {
            for_all_versions(*app, [&] {
                auto tx = root.tx({createAccount(f, startAmount)});
                validateTxResults(tx, *app, {baseFee * 1, txSUCCESS},
                                  expectedResult(baseFee * 1, 1, txSUCCESS,
                                                 {CREATE_ACCOUNT_SUCCESS}));
            });
        }

        SECTION("with payment after")
        {
            for_all_versions(*app, [&] {
                auto tx = root.tx({createAccount(f, startAmount),
                                   a.op(payment(root, startAmount / 2))});
                tx->addSignature(a);
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {CREATE_ACCOUNT_SUCCESS, PAYMENT_SUCCESS}));
            });
        }
    }

    SECTION("not enough signature weight")
    {
        auto th = setMasterWeight(10) | setLowThreshold(10) |
                  setMedThreshold(50) | setHighThreshold(100);

        SECTION("normal")
        {
            for_all_versions(*app, [&] {
                auto tx = a.tx({payment(b, 1000), setOptions(th)});
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("with operation after")
        {
            for_versions_to(6, *app, [&] {
                auto tx =
                    a.tx({payment(b, 1000), setOptions(th), payment(c, 1000)});
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(
                        baseFee * 3, 3, txFAILED,
                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions({8, 9}, *app, [&] {
                auto tx =
                    a.tx({payment(b, 1000), setOptions(th), payment(c, 1000)});
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(
                        baseFee * 3, 3, txFAILED,
                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
                auto tx =
                    a.tx({payment(b, 1000), setOptions(th), payment(c, 1000)});
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(baseFee * 3, 3, txSUCCESS,
                                   {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS,
                                    PAYMENT_SUCCESS}));
            });
        }

        SECTION("before tx")
        {
            a.setOptions(th);
            for_versions_to(6, *app, [&] {
                auto tx = a.tx({payment(b, 1000)});
                validateTxResults(tx, *app, {baseFee * 1, txFAILED});
            });
            for_versions({7}, *app, [&] {
                auto tx = a.tx({payment(b, 1000)});
                validateTxResults(tx, *app, {baseFee * 1, txSUCCESS},
                                  expectedResult(baseFee * 1, 1, txSUCCESS,
                                                 {PAYMENT_SUCCESS}));
            });
            for_versions_from(8, *app, [&] {
                auto tx = a.tx({payment(b, 1000)});
                validateTxResults(tx, *app, {baseFee * 1, txFAILED});
            });
        }
    }

    SECTION("fees with liabilities")
    {
        auto acc = root.create("acc", lm.getLastMinBalance(1) + baseFee + 1000);
        auto native = makeNativeAsset();
        auto cur1 = acc.asset("CUR1");

        TestMarket market(*app);
        SECTION("selling liabilities")
        {
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc, {native, cur1, Price{1, 1}, 1000});
            });
            for_versions_to(9, *app, [&] {
                auto tx = acc.tx({payment(root, 1)});
                auto res =
                    expectedResult(baseFee, 1, txSUCCESS, {PAYMENT_SUCCESS});
                validateTxResults(tx, *app, {baseFee, txSUCCESS}, res);
            });
            for_versions_from(10, *app, [&] {
                auto tx = acc.tx({payment(root, 1)});
                validateTxResults(tx, *app, {baseFee, txINSUFFICIENT_BALANCE});
            });
        }
        SECTION("buying liabilities")
        {
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc, {cur1, native, Price{1, 1}, 1000});
            });
            for_all_versions(*app, [&] {
                auto tx = acc.tx({payment(root, 1)});
                auto res =
                    expectedResult(baseFee, 1, txSUCCESS, {PAYMENT_SUCCESS});
                validateTxResults(tx, *app, {baseFee, txSUCCESS}, res);
            });
        }
    }
}
