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

    auto success = expectedResult(100, txSUCCESS, {PAYMENT_SUCCESS}, txSUCCESS,
                                  {PAYMENT_SUCCESS});
    auto fastFailure = [](TransactionResultCode code, int fee) {
        return expectedResult(fee, code, {}, code, {});
    };
    auto failure = [](TransactionResultCode validation,
                      TransactionResultCode apply) {
        return expectedResult(100, validation, {}, apply, {});
    };
    auto failed = [](TransactionResultCode apply) {
        if (apply == txFAILED)
        {
            return expectedResult(100, txFAILED, {PAYMENT_MALFORMED}, txFAILED,
                                  {PAYMENT_MALFORMED});
        }
        else
        {
            return expectedResult(100, txFAILED, {PAYMENT_MALFORMED}, apply,
                                  {});
        }
    };

    SECTION("transaction errors")
    {
        for (auto error :
             {txMISSING_OPERATION, txTOO_EARLY, txTOO_LATE, txINSUFFICIENT_FEE})
        {
            SECTION(xdr::xdr_traits<TransactionResultCode>::enum_name(error))
            {
                for_all_versions(*app, [&] {
                    auto tx = buildTransactionWithError(error);
                    applyCheck(tx, *app, fastFailure(error, 1));
                });
            }
        }

        SECTION("txNO_ACCOUNT")
        {
            for_all_versions(*app, [&] {
                auto tx = buildTransactionWithError(txNO_ACCOUNT);
                applyCheck(tx, *app, fastFailure(txNO_ACCOUNT, 100));
            });
        }

        SECTION("txINSUFFICIENT_BALANCE")
        {
            auto tx = buildTransactionWithError(txINSUFFICIENT_BALANCE);
            for_versions_to(9, *app, [&] {
                applyCheck(
                    tx, *app,
                    failure(txINSUFFICIENT_BALANCE, txINSUFFICIENT_BALANCE));
            });
            for_versions_from(10, *app, [&] {
                applyCheck(tx, *app,
                           failure(txINSUFFICIENT_BALANCE, txBAD_AUTH_EXTRA));
            });
        }

        SECTION("txBAD_SEQ")
        {
            auto tx = buildTransactionWithError(txBAD_SEQ);
            for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
                applyCheck(tx, *app, failure(txBAD_SEQ, txBAD_AUTH));
            });
            for_versions({7}, *app, [&] {
                applyCheck(tx, *app,
                           failure(txBAD_SEQ, txINSUFFICIENT_BALANCE));
            });
            for_versions_from(10, *app, [&] {
                applyCheck(tx, *app, failure(txBAD_SEQ, txBAD_SEQ));
            });
        }

        SECTION("txBAD_AUTH")
        {
            auto tx = buildTransactionWithError(txBAD_AUTH);
            for_versions({7}, *app, [&] {
                applyCheck(
                    tx, *app,
                    failure(txINSUFFICIENT_BALANCE, txINSUFFICIENT_BALANCE));
            });

            for_all_versions_except({7}, *app, [&] {
                applyCheck(tx, *app, failure(txBAD_AUTH, txBAD_AUTH));
            });
        }

        SECTION("txBAD_AUTH_EXTRA")
        {
            auto tx = buildTransactionWithError(txBAD_AUTH_EXTRA);
            for_versions({7}, *app, [&] { applyCheck(tx, *app, success); });

            for_all_versions_except({7}, *app, [&] {
                applyCheck(tx, *app,
                           failure(txBAD_AUTH_EXTRA, txBAD_AUTH_EXTRA));
            });
        }

        SECTION("txFAILED")
        {
            auto tx = buildTransactionWithError(txFAILED);
            for_versions_to(9, *app,
                            [&] { applyCheck(tx, *app, failed(txFAILED)); });
            for_versions_from(10, *app, [&] {
                applyCheck(tx, *app, failed(txBAD_AUTH_EXTRA));
            });
        }

        SECTION("txSUCCESS")
        {
            auto tx = buildTransactionWithError(txSUCCESS);
            for_all_versions(*app, [&] { applyCheck(tx, *app, success); });
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

    auto all_of = [&](std::vector<Signed> const& opSigned,
                      std::vector<PaymentValidity> const& opPayment,
                      std::function<bool(Signed, PaymentValidity)> const& f) {
        assert(opSigned.size() == opPayment.size());
        for (size_t i = 0; i < opPayment.size(); i++)
        {
            if (!(f(opSigned[i], opPayment[i])))
            {
                return false;
            }
        }
        return true;
    };

    auto any_of = [&](std::vector<Signed> const& opSigned,
                      std::vector<PaymentValidity> const& opPayment,
                      std::function<bool(Signed, PaymentValidity)> const& f) {
        return !all_of(opSigned, opPayment, [&](auto sig, auto payment) {
            return !f(sig, payment);
        });
    };

    auto fillWithCreateAccountSuccess = [](std::vector<ExpectedOpResult>& ops,
                                           int count) {
        for (size_t i = ops.size(); i < count; i++)
        {
            ops.push_back(CREATE_ACCOUNT_SUCCESS);
        }
    };

    auto fillWithCreateAccountSuccessOrBadAuth =
        [](std::vector<ExpectedOpResult>& ops,
           std::vector<Signed> const& opSigned) {
            for (size_t i = ops.size(); i < opSigned.size(); i++)
            {
                if (opSigned[i] == Signed::NOT_SIGNED)
                {
                    ops.push_back(opBAD_AUTH);
                }
                else
                {
                    ops.push_back(CREATE_ACCOUNT_SUCCESS);
                }
            }
        };

    auto makeSimpleValidationOperationResult =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            assert(opSigned.size() == opPayment.size());
            auto ops = std::vector<ExpectedOpResult>{};
            for (size_t i = 0; i < opPayment.size(); i++)
            {
                if (opSigned[i] == Signed::NOT_SIGNED)
                {
                    ops.push_back(opBAD_AUTH);
                    break;
                }

                if (opPayment[i] == PaymentValidity::MALFORMED)
                {
                    ops.push_back(PAYMENT_MALFORMED);
                    break;
                }

                ops.push_back(PAYMENT_SUCCESS);
            }
            fillWithCreateAccountSuccess(ops, opPayment.size());
            return ops;
        };

    auto makeValidationResult =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment, bool doubleSigned) {
            assert(opSigned.size() == opPayment.size());
            if (opSigned[0] == Signed::NOT_SIGNED)
            {
                return std::make_pair(txBAD_AUTH,
                                      std::vector<ExpectedOpResult>{});
            }

            auto success =
                all_of(opSigned, opPayment, [](auto sig, auto payment) {
                    return sig != Signed::NOT_SIGNED &&
                           payment != PaymentValidity::MALFORMED;
                });
            auto ops = makeSimpleValidationOperationResult(opSigned, opPayment);
            if (success && doubleSigned)
            {
                return std::make_pair(txBAD_AUTH_EXTRA,
                                      std::vector<ExpectedOpResult>{});
            }
            else
            {
                return std::make_pair(success ? txSUCCESS : txFAILED, ops);
            }
        };

    auto makeValidationResultBefore10 =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            auto doubleSigned =
                any_of(opSigned, opPayment, [](auto sig, auto payment) {
                    return sig == Signed::DOUBLE_SIGNED &&
                           payment != PaymentValidity::MALFORMED;
                });
            return makeValidationResult(opSigned, opPayment, doubleSigned);
        };

    auto makeValidationResultOn7 =
        [&](std::vector<Signed> opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            for (auto& s : opSigned)
            {
                s = Signed::SIGNED;
            }

            return makeValidationResultBefore10(opSigned, opPayment);
        };

    auto makeValidationResultOnAndAfter10 =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            auto doubleSigned =
                any_of(opSigned, opPayment, [](auto sig, auto payment) {
                    return sig == Signed::DOUBLE_SIGNED;
                });
            return makeValidationResult(opSigned, opPayment, doubleSigned);

        };

    auto makeApplyOps = [&](std::vector<Signed> const& opSigned,
                            std::vector<PaymentValidity> const& opPayment,
                            bool continueOnNonSigned) {
        assert(opSigned.size() == opPayment.size());

        auto fundNextOperation = false;
        auto ops = std::vector<ExpectedOpResult>{};
        for (size_t i = 0; i < opPayment.size(); i++)
        {
            auto currentOperationFunded = fundNextOperation;
            fundNextOperation = false;
            if (opSigned[i] == Signed::NOT_SIGNED)
            {
                ops.push_back(opBAD_AUTH);
                if (continueOnNonSigned)
                {
                    continue;
                }
                else
                {
                    break;
                }
            }

            switch (opPayment[i])
            {
            case PaymentValidity::UNDERFUNDED:
                if (!currentOperationFunded)
                {
                    ops.push_back(PAYMENT_UNDERFUNDED);
                }
                else
                {
                    ops.push_back(PAYMENT_SUCCESS);
                }
                fundNextOperation = true;
                break;
            case PaymentValidity::MALFORMED:
                ops.push_back(PAYMENT_MALFORMED);
                break;
            case PaymentValidity::VALID:
                ops.push_back(PAYMENT_SUCCESS);
                break;
            default:
                break;
            }
        }

        return ops;
    };

    auto makeApplyResultBefore10 =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            assert(opSigned.size() == opPayment.size());

            auto validationResult =
                makeValidationResultBefore10(opSigned, opPayment);
            if (validationResult.first != txSUCCESS &&
                validationResult.first != txFAILED &&
                validationResult.first != txBAD_AUTH_EXTRA)
            {
                return validationResult;
            }

            auto success =
                all_of(opSigned, opPayment, [](auto sig, auto payment) {
                    return sig != Signed::NOT_SIGNED &&
                           payment == PaymentValidity::VALID;
                });

            if (success)
            {
                auto doubleSigned =
                    any_of(opSigned, opPayment, [](auto sig, auto payment) {
                        return sig == Signed::DOUBLE_SIGNED &&
                               payment != PaymentValidity::MALFORMED;
                    });
                if (doubleSigned)
                {
                    return std::make_pair(txBAD_AUTH_EXTRA,
                                          std::vector<ExpectedOpResult>{});
                }
            }

            auto ops = makeApplyOps(opSigned, opPayment, true);
            return std::make_pair(success ? txSUCCESS : txFAILED, ops);
        };

    auto makeApplyResultOn7 =
        [&](std::vector<Signed> opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            for (auto& s : opSigned)
            {
                s = Signed::SIGNED;
            }

            return makeApplyResultBefore10(opSigned, opPayment);
        };

    auto makeApplyResultOnAndAfter10 =
        [&](std::vector<Signed> const& opSigned,
            std::vector<PaymentValidity> const& opPayment) {
            assert(opSigned.size() == opPayment.size());

            auto validationResult =
                makeValidationResultOnAndAfter10(opSigned, opPayment);
            if (validationResult.first != txSUCCESS &&
                validationResult.first != txFAILED &&
                validationResult.first != txBAD_AUTH_EXTRA)
            {
                return validationResult;
            }

            auto success =
                all_of(opSigned, opPayment, [](auto sig, auto payment) {
                    return sig != Signed::NOT_SIGNED &&
                           payment == PaymentValidity::VALID;
                });

            auto anyBadAuth =
                std::find(std::begin(opSigned), std::end(opSigned),
                          Signed::NOT_SIGNED) != std::end(opSigned);
            if (anyBadAuth)
            {
                auto ops = std::vector<ExpectedOpResult>{};
                for (size_t i = 0; i < opPayment.size(); i++)
                {
                    if (opSigned[i] == Signed::NOT_SIGNED)
                    {
                        ops.push_back(opBAD_AUTH);
                        break;
                    }

                    if (opPayment[i] == PaymentValidity::MALFORMED)
                    {
                        ops.push_back(PAYMENT_MALFORMED);
                        break;
                    }

                    ops.push_back(PAYMENT_SUCCESS);
                }

                fillWithCreateAccountSuccessOrBadAuth(ops, opSigned);
                return std::make_pair(success ? txSUCCESS : txFAILED, ops);
            }
            else
            {
                auto doubleSigned =
                    any_of(opSigned, opPayment, [](auto sig, auto payment) {
                        return sig == Signed::DOUBLE_SIGNED;
                    });
                if (doubleSigned)
                {
                    return std::make_pair(txBAD_AUTH_EXTRA,
                                          std::vector<ExpectedOpResult>{});
                }
            }

            auto ops = makeApplyOps(opSigned, opPayment, false);
            fillWithCreateAccountSuccessOrBadAuth(ops, opSigned);
            return std::make_pair(success ? txSUCCESS : txFAILED, ops);
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
            auto destination = accounts[(i + 1) % accounts.size()];
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
        auto fee = static_cast<int64_t>(baseFee * ops.size());
        auto tx = makeTx(signs, ops);

        for_versions({1, 2, 3, 4, 5, 6, 8, 9}, *app, [&] {
            auto validationResult = makeValidationResultBefore10(signs, ops);
            auto applyResult = makeApplyResultBefore10(signs, ops);
            auto result = expectedResult(fee, validationResult.first,
                                         validationResult.second,
                                         applyResult.first, applyResult.second);

            applyCheck(tx, *app, result, true);
        });

        for_versions({7}, *app, [&] {
            auto validationResult = makeValidationResultOn7(signs, ops);
            auto applyResult = makeApplyResultOn7(signs, ops);
            auto result = expectedResult(fee, validationResult.first,
                                         validationResult.second,
                                         applyResult.first, applyResult.second);
            applyCheck(tx, *app, result, true);
        });

        for_versions_from(10, *app, [&] {
            uint32_t ledgerVersion = 0;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ledgerVersion = ltx.loadHeader().current().ledgerVersion;
            }
            auto validationResult =
                makeValidationResultOnAndAfter10(signs, ops);
            auto applyResult = makeApplyResultOnAndAfter10(signs, ops);
            auto result = expectedResult(fee, validationResult.first,
                                         validationResult.second,
                                         applyResult.first, applyResult.second);
            applyCheck(tx, *app, result, true);
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

            auto result = expectedResult(
                baseFee * 2, txSUCCESS,
                {PAYMENT_SUCCESS, {ACCOUNT_MERGE_SUCCESS, 0}}, txSUCCESS,
                {PAYMENT_SUCCESS, {ACCOUNT_MERGE_SUCCESS, startAmount - 1200}});
            for_all_versions(*app, [&] { applyCheck(tx, *app, result); });
        }

        SECTION("with operation after")
        {
            auto tx =
                a.tx({payment(b, 1000), accountMerge(root), payment(c, 1000)});

            for_versions_to(7, *app, [&] {
                auto result = expectedResult(baseFee * 3, txSUCCESS,
                                             {PAYMENT_SUCCESS,
                                              {ACCOUNT_MERGE_SUCCESS, 0},
                                              PAYMENT_SUCCESS},
                                             txINTERNAL_ERROR, {});
                applyCheck(tx, *app, result);
            });
            for_versions_from(8, *app, [&] {
                auto result =
                    expectedResult(baseFee * 3, txSUCCESS,
                                   {PAYMENT_SUCCESS,
                                    {ACCOUNT_MERGE_SUCCESS, 0},
                                    PAYMENT_SUCCESS},
                                   txFAILED,
                                   {PAYMENT_SUCCESS,
                                    {ACCOUNT_MERGE_SUCCESS,
                                     startAmount - tx->getFeeBid() - 1000},
                                    opNO_ACCOUNT});
                applyCheck(tx, *app, result);
            });
        }
    }

    SECTION("create account")
    {
        SECTION("normal")
        {
            auto tx = root.tx({createAccount(f, startAmount)});

            auto result =
                expectedResult(baseFee, txSUCCESS, {CREATE_ACCOUNT_SUCCESS},
                               txSUCCESS, {CREATE_ACCOUNT_SUCCESS});
            for_all_versions(*app, [&] { applyCheck(tx, *app, result); });
        }

        SECTION("with payment after")
        {
            auto tx = root.tx({createAccount(f, startAmount),
                               a.op(payment(root, startAmount / 2))});
            a.sign(tx);

            auto result = expectedResult(
                baseFee * 2, txSUCCESS,
                {CREATE_ACCOUNT_SUCCESS, PAYMENT_SUCCESS}, txSUCCESS,
                {CREATE_ACCOUNT_SUCCESS, PAYMENT_SUCCESS});
            for_all_versions(*app, [&] { applyCheck(tx, *app, result); });
        }
    }

    SECTION("not enough signature weight")
    {
        auto th = setMasterWeight(10) | setLowThreshold(10) |
                  setMedThreshold(50) | setHighThreshold(100);

        SECTION("normal")
        {
            auto tx = a.tx({payment(b, 1000), setOptions(th)});
            auto result = expectedResult(
                baseFee * 2, txSUCCESS, {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS},
                txSUCCESS, {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS});
            for_all_versions(*app, [&] { applyCheck(tx, *app, result); });
        }

        SECTION("with operation after")
        {
            auto tx =
                a.tx({payment(b, 1000), setOptions(th), payment(c, 1000)});
            for_versions_to(6, *app, [&] {
                auto result = expectedResult(
                    baseFee * 3, txSUCCESS,
                    {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, PAYMENT_SUCCESS},
                    txFAILED,
                    {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, opBAD_AUTH});
                applyCheck(tx, *app, result);
            });
            for_versions({8, 9}, *app, [&] {
                auto result = expectedResult(
                    baseFee * 3, txSUCCESS,
                    {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, PAYMENT_SUCCESS},
                    txFAILED,
                    {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, opBAD_AUTH});
                applyCheck(tx, *app, result);
            });
            for_versions_from({7, 10}, *app, [&] {
                auto result = expectedResult(
                    baseFee * 3, txSUCCESS,
                    {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, PAYMENT_SUCCESS},
                    txSUCCESS,
                    {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS, PAYMENT_SUCCESS});
                applyCheck(tx, *app, result);
            });
        }

        SECTION("before tx")
        {
            a.setOptions(th);
            auto tx = a.tx({payment(b, 1000)});

            for_versions({7}, *app, [&] {
                auto result =
                    expectedResult(baseFee, txSUCCESS, {PAYMENT_SUCCESS},
                                   txSUCCESS, {PAYMENT_SUCCESS});
                applyCheck(tx, *app, result);
            });
            for_all_versions_except({7}, *app, [&] {
                auto result = expectedResult(baseFee, txFAILED, {opBAD_AUTH},
                                             txFAILED, {opBAD_AUTH});
                applyCheck(tx, *app, result);
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
                auto result =
                    expectedResult(baseFee, txSUCCESS, {PAYMENT_SUCCESS},
                                   txSUCCESS, {PAYMENT_SUCCESS});
                applyCheck(tx, *app, result);
            });
            for_versions_from(10, *app, [&] {
                auto result = expectedResult(baseFee, txINSUFFICIENT_BALANCE,
                                             {}, txINSUFFICIENT_BALANCE, {});
                applyCheck(tx, *app, result);
            });
        }
        SECTION("buying liabilities")
        {
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc, {cur1, native, Price{1, 1}, 1000});
            });
            auto tx = acc.tx({payment(root, 1)});
            for_all_versions(*app, [&] {
                auto result =
                    expectedResult(baseFee, txSUCCESS, {PAYMENT_SUCCESS},
                                   txSUCCESS, {PAYMENT_SUCCESS});
                applyCheck(tx, *app, result);
            });
        }
    }
}
