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
#include "util/Timer.h"

#include <algorithm>
#include <lib/catch.hpp>
#include <xdrpp/printer.h>

using namespace stellar;
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

static auto signedTypes = std::vector<Signed>{
    Signed::NOT_SIGNED, Signed::SIGNED, Signed::DOUBLE_SIGNED};
static auto signedNames =
    std::map<Signed, std::string>{{Signed::NOT_SIGNED, "not signed"},
                                  {Signed::SIGNED, "signed"},
                                  {Signed::DOUBLE_SIGNED, "double signed"}};
static auto paymentValidityTypes = std::vector<PaymentValidity>{
    PaymentValidity::VALID, PaymentValidity::MALFORMED,
    PaymentValidity::UNDERFUNDED};
static auto paymentValidityNames = std::map<PaymentValidity, std::string>{
    {PaymentValidity::VALID, "valid"},
    {PaymentValidity::MALFORMED, "malformed"},
    {PaymentValidity::UNDERFUNDED, "underfunded"}};
}

template <typename T>
std::vector<std::vector<T>>
variations(int count, std::vector<T> values)
{
    if (count == 0)
    {
        return {{}};
    }

    auto result = std::vector<std::vector<T>>{};
    auto sub = variations(count - 1, values);
    for (auto s : sub)
    {
        for (auto v : values)
        {
            result.push_back(s);
            result.back().push_back(v);
        }
    }

    return result;
}

TEST_CASE("txresults with errors", "[tx][txresults]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    const int64_t baseReserve = lm.getLastReserve();
    const int64_t startAmount = baseReserve * 100;

    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        ltx.loadHeader().current().scpValue.closeTime = 10;
        ltx.commit();
    }

    auto closeTime = [&] {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        return ltx.loadHeader().current().scpValue.closeTime;
    }();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto account = root.create("account", startAmount);

    auto buildTransactionWithError = [&](TransactionResultCode failFromCode) {
        auto rawTx = account.rawTx({payment(root, 1)});

        auto noAccount = false;
        auto badAuth = false;
        auto extraAuth = false;

        // NB: cases are fall through here as we want to add all possible
        // negative conditions if we expect a failure at a certain point
        switch (failFromCode)
        {
        case txMISSING_OPERATION:
            rawTx.operations.clear();
        case txTOO_EARLY:
            rawTx.timeBounds.activate().minTime = closeTime + 1;
        case txTOO_LATE:
            rawTx.timeBounds.activate().maxTime = closeTime - 1;
        case txINSUFFICIENT_FEE:
            rawTx.fee = 1;
        case txNO_ACCOUNT:
            noAccount = true;
        case txBAD_SEQ:
            rawTx.seqNum++;
        case txBAD_AUTH:
            badAuth = true;
        case txINSUFFICIENT_BALANCE:
            if (!noAccount)
            {
                auto payTx = root.tx({account.op(txtest::payment(
                    root, account.getBalance() - 2 * baseReserve))});
                account.sign(payTx);
                applyTx(payTx, *app);
            }
        case txFAILED:
            if (!rawTx.operations.empty())
            {
                rawTx.operations[0].body.paymentOp().amount = 0;
            }
        case txBAD_AUTH_EXTRA:
            extraAuth = true;
        case txSUCCESS:
            break;
        case txINTERNAL_ERROR:
            assert(false);
            break;
        };

        if (noAccount)
        {
            rawTx.sourceAccount = TestAccount{*app, getAccount("nonExisting")};
        }
        auto tx = (badAuth ? TestAccount{*app, getAccount("badAuth")} : account)
                      .tx(rawTx);
        if (extraAuth)
        {
            TestAccount{*app, getAccount("extraSignature")}.sign(tx);
        }

        return tx;
    };

    auto validateError = [&](std::shared_ptr<TransactionFrame> const& tx,
                             TransactionResultCode failFromCode) {
        auto version = [&]() {
            LedgerTxn ltx(app->getLedgerTxnRoot());
            return ltx.loadHeader().current().ledgerVersion;
        }();
        if (failFromCode == txSUCCESS ||
            (version == 7 && failFromCode == txBAD_AUTH_EXTRA))
        {
            validateTxResults(
                tx, *app, {tx->getFeeBid(), txSUCCESS},
                expectedResult(100, 1, txSUCCESS, {PAYMENT_SUCCESS}));
        }
        else if (version == 7 && failFromCode == txBAD_AUTH)
        {
            validateTxResults(tx, *app,
                              {tx->getFeeBid(), txINSUFFICIENT_BALANCE});
        }
        else
        {
            validateTxResults(tx, *app, {tx->getFeeBid(), failFromCode});
        }
    };

    SECTION("transaction errors")
    {
        for (auto error :
             {txMISSING_OPERATION, txTOO_EARLY, txTOO_LATE, txINSUFFICIENT_FEE,
              txNO_ACCOUNT, txBAD_SEQ, txBAD_AUTH, txINSUFFICIENT_BALANCE,
              txFAILED, txBAD_AUTH_EXTRA, txSUCCESS})
        {
            SECTION(xdr::xdr_traits<TransactionResultCode>::enum_name(error))
            {
                for_all_versions(*app, [&] {
                    auto tx = buildTransactionWithError(error);
                    validateError(tx, error);
                });
            }
        }
    }
}

TEST_CASE("complex txresults", "[tx][txresults]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    const int64_t baseReserve = lm.getLastReserve();
    const int64_t baseFee = lm.getLastTxFee();
    const int64_t startAmount = baseReserve * 100;

    auto amount = [&](PaymentValidity t) {
        switch (t)
        {
        case PaymentValidity::VALID:
            return int64_t{1000};
        case PaymentValidity::MALFORMED:
            return int64_t{0};
        case PaymentValidity::UNDERFUNDED:
            return startAmount * 2;
        default:
            abort();
        }
    };

    auto sign = [&](TransactionFramePtr const& tx, TestAccount& a, Signed t) {
        switch (t)
        {
        case Signed::NOT_SIGNED:
            return;
        case Signed::SIGNED:
            a.sign(tx);
            return;
        case Signed::DOUBLE_SIGNED:
            a.sign(tx);
            a.sign(tx);
            return;
        }
    };

    auto makeValidationResult =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment, int ledgerVersion) {
            assert(opSigned.size() == opPayment.size());

            auto fee = static_cast<int64_t>(baseFee * opPayment.size());
            auto doubleSigned = false;
            if (ledgerVersion != 7)
            {
                switch (opSigned[0])
                {
                case Signed::NOT_SIGNED:
                    return ValidationResult{fee, txBAD_AUTH};
                case Signed::DOUBLE_SIGNED:
                    doubleSigned = true;
                    break;
                default:
                    break;
                }
            }

            if (ledgerVersion != 7)
            {
                for (size_t i = 1; i < opSigned.size(); i++)
                {
                    switch (opSigned[i])
                    {
                    case Signed::NOT_SIGNED:
                        return ValidationResult{fee, txFAILED};
                    case Signed::DOUBLE_SIGNED:
                        doubleSigned = true;
                        break;
                    default:
                        break;
                    }
                }
            }

            for (auto opP : opPayment)
            {
                switch (opP)
                {
                case PaymentValidity::MALFORMED:
                    return ValidationResult{fee, txFAILED};
                default:
                    break;
                }
            }

            if (doubleSigned && (ledgerVersion != 7))
            {
                return ValidationResult{fee, txBAD_AUTH_EXTRA};
            }
            else
            {
                return ValidationResult{fee, txSUCCESS};
            }
        };

    auto makeApplyResult = [&](std::vector<Signed> const& opSigned,
                               std::vector<PaymentValidity> const& opPayment,
                               int ledgerVersion) {
        assert(opSigned.size() == opPayment.size());

        auto fee = static_cast<int64_t>(baseFee * opPayment.size());
        auto validationResult =
            makeValidationResult(opSigned, opPayment, ledgerVersion);
        if (validationResult.code != txSUCCESS)
        {
            return TransactionResult{};
        }

        auto opResults = std::vector<ExpectedOpResult>{};
        auto firstUnderfunded = true;

        for (size_t i = 0; i < opPayment.size(); i++)
        {
            if (ledgerVersion != 7)
            {
                REQUIRE(opSigned[i] == Signed::SIGNED);
            }
            REQUIRE(opPayment[i] != PaymentValidity::MALFORMED);

            switch (opPayment[i])
            {
            case PaymentValidity::UNDERFUNDED:
                if (firstUnderfunded)
                {
                    opResults.push_back(PAYMENT_UNDERFUNDED);
                    firstUnderfunded = false;
                }
                else
                {
                    opResults.push_back(PAYMENT_SUCCESS);
                }
                break;
            case PaymentValidity::VALID:
                opResults.push_back(PAYMENT_SUCCESS);
                firstUnderfunded = true; // I have no idea
                break;
            default:
                break;
            }
        }

        auto anyFail = std::any_of(
            std::begin(opResults), std::end(opResults), [](ExpectedOpResult o) {
                return o.mOperationResult.code() != opINNER ||
                       o.mOperationResult.tr().paymentResult().code() !=
                           PAYMENT_SUCCESS;
            });
        return expectedResult(fee, opPayment.size(),
                              anyFail ? txFAILED : validationResult.code,
                              opResults);
    };

    auto sign3 = variations<Signed>(3, signedTypes);
    auto op3 = variations<PaymentValidity>(3, paymentValidityTypes);

    auto signSectionName = [](std::vector<Signed> const& signs) {
        auto result = std::string{"tx and op1 " + signedNames[signs[0]]};
        for (size_t i = 1; i < signs.size(); i++)
            result +=
                ", op" + std::to_string(i + 1) + " " + signedNames[signs[i]];
        return result;
    };
    auto opSectionName = [](std::vector<PaymentValidity> const& ops) {
        auto result = std::string{"op1 " + paymentValidityNames[ops[0]]};
        for (size_t i = 1; i < ops.size(); i++)
            result += ", op" + std::to_string(i + 1) + " " +
                      paymentValidityNames[ops[i]];
        return result;
    };

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto a = root.create("a", startAmount);
    auto b = root.create("b", startAmount);
    auto c = root.create("c", startAmount);
    auto d = root.create("d", startAmount);
    auto e = root.create("e", startAmount);
    auto f = TestAccount{*app, getAccount("f")};

    auto accounts = std::vector<TestAccount*>{&a, &b, &c, &d, &e};
    auto makeTx = [&](std::vector<Signed> const& signs,
                      std::vector<PaymentValidity> const& ops) {
        auto operations = std::vector<Operation>{};
        for (size_t i = 0; i < ops.size(); i++)
        {
            auto destination = accounts[(i + 1) % ops.size()];
            auto op = payment(*destination, amount(ops[i]));
            if (i != 0)
                op = accounts[i]->op(op);
            operations.push_back(op);
        }

        auto tx = a.unsignedTx(operations);

        for (size_t i = 0; i < signs.size(); i++)
        {
            sign(tx, *accounts[i], signs[i]);
        }

        return tx;
    };

    auto test = [&](std::vector<Signed> const& signs,
                    std::vector<PaymentValidity> const& ops) {
        auto tx = makeTx(signs, ops);
        for_all_versions(*app, [&] {
            uint32_t ledgerVersion = 0;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ledgerVersion = ltx.loadHeader().current().ledgerVersion;
            }
            auto validationResult =
                makeValidationResult(signs, ops, ledgerVersion);
            auto applyResult = makeApplyResult(signs, ops, ledgerVersion);
            validateTxResults(tx, *app, validationResult, applyResult);
        });
    };

    for (auto signs : sign3)
    {
        SECTION(signSectionName(signs))
        {
            for (auto ops : op3)
            {
                SECTION(opSectionName(ops))
                {
                    test(signs, ops);
                }
            }
        }
    }

    SECTION("5 signed but underfunded")
    {
        test({Signed::SIGNED, Signed::SIGNED, Signed::SIGNED, Signed::SIGNED,
              Signed::SIGNED},
             {PaymentValidity::UNDERFUNDED, PaymentValidity::UNDERFUNDED,
              PaymentValidity::UNDERFUNDED, PaymentValidity::UNDERFUNDED,
              PaymentValidity::UNDERFUNDED});
    }

    SECTION("merge account")
    {
        closeLedgerOn(*app, 2, 1, 1, 2016);
        SECTION("normal")
        {
            auto tx = a.tx({payment(b, 1000), accountMerge(root)});

            auto applyResult = expectedResult(
                baseFee * 2, 2, txSUCCESS,
                {PAYMENT_SUCCESS, {ACCOUNT_MERGE_SUCCESS, startAmount - 1200}});
            for_all_versions(*app, [&] {
                validateTxResults(tx, *app, {baseFee * 2, txSUCCESS},
                                  applyResult);
            });
        }

        SECTION("with operation after")
        {
            auto tx =
                a.tx({payment(b, 1000), accountMerge(root), payment(c, 1000)});

            for_versions_to(7, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(baseFee * 3, 3, txINTERNAL_ERROR));
            });
            for_versions_from(8, *app, [&] {
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
            auto tx = root.tx({createAccount(f, startAmount)});

            for_all_versions(*app, [&] {
                validateTxResults(tx, *app, {baseFee * 1, txSUCCESS},
                                  expectedResult(baseFee * 1, 1, txSUCCESS,
                                                 {CREATE_ACCOUNT_SUCCESS}));
            });
        }

        SECTION("with payment after")
        {
            auto tx = root.tx({createAccount(f, startAmount),
                               a.op(payment(root, startAmount / 2))});
            a.sign(tx);

            for_all_versions(*app, [&] {
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
            auto tx = a.tx({payment(b, 1000), setOptions(th)});
            for_all_versions(*app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("with operation after")
        {
            auto tx =
                a.tx({payment(b, 1000), setOptions(th), payment(c, 1000)});

            for_versions_to(6, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(
                        baseFee * 3, 3, txFAILED,
                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions({8, 9}, *app, [&] {
                validateTxResults(
                    tx, *app, {baseFee * 3, txSUCCESS},
                    expectedResult(
                        baseFee * 3, 3, txFAILED,
                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, opBAD_AUTH}));
            });
            for_versions_from({7, 10}, *app, [&] {
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
            auto tx = a.tx({payment(b, 1000)});

            for_versions_to(6, *app, [&] {
                validateTxResults(tx, *app, {baseFee * 1, txFAILED});
            });
            for_versions({7}, *app, [&] {
                validateTxResults(tx, *app, {baseFee * 1, txSUCCESS},
                                  expectedResult(baseFee * 1, 1, txSUCCESS,
                                                 {PAYMENT_SUCCESS}));
            });
            for_versions_from(8, *app, [&] {
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
            auto tx = acc.tx({payment(root, 1)});
            for_versions_to(9, *app, [&] {
                auto res =
                    expectedResult(baseFee, 1, txSUCCESS, {PAYMENT_SUCCESS});
                validateTxResults(tx, *app, {baseFee, txSUCCESS}, res);
            });
            for_versions_from(10, *app, [&] {
                validateTxResults(tx, *app, {baseFee, txINSUFFICIENT_BALANCE});
            });
        }
        SECTION("buying liabilities")
        {
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc, {cur1, native, Price{1, 1}, 1000});
            });
            auto tx = acc.tx({payment(root, 1)});
            for_all_versions(*app, [&] {
                auto res =
                    expectedResult(baseFee, 1, txSUCCESS, {PAYMENT_SUCCESS});
                validateTxResults(tx, *app, {baseFee, txSUCCESS}, res);
            });
        }
    }
}
