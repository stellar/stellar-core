// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/SignerKey.h"
#include "ledger/LedgerDelta.h"
#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Timer.h"
#include "xdrpp/printer.h"
#include <algorithm>

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

struct ExpectedOpResult
{
    OperationResultCode code;
    OperationType type;
    CreateAccountResultCode createAccountCode;
    PaymentResultCode paymentCode;
    AccountMergeResultCode accountMergeCode;
    SetOptionsResultCode setOptionsResultCode;

    ExpectedOpResult(OperationResultCode code) : code{code}
    {
    }
    ExpectedOpResult(CreateAccountResultCode createAccountCode)
        : code{opINNER}
        , type{CREATE_ACCOUNT}
        , createAccountCode{createAccountCode}
    {
    }
    ExpectedOpResult(PaymentResultCode paymentCode)
        : code{opINNER}, type{PAYMENT}, paymentCode{paymentCode}
    {
    }
    ExpectedOpResult(AccountMergeResultCode accountMergeCode)
        : code{opINNER}, type{ACCOUNT_MERGE}, accountMergeCode{accountMergeCode}
    {
    }
    ExpectedOpResult(SetOptionsResultCode setOptionsResultCode)
        : code{opINNER}
        , type{SET_OPTIONS}
        , setOptionsResultCode{setOptionsResultCode}
    {
    }
};

TransactionResult
expectedResult(int64_t fee, size_t opsCount, TransactionResultCode code,
               std::vector<ExpectedOpResult> ops = {})
{
    auto result = TransactionResult{};
    result.feeCharged = fee;
    result.result.code(code);
    if (code != txSUCCESS && code != txFAILED)
    {
        return result;
    }
    if (ops.empty())
    {
        std::fill_n(std::back_inserter(ops), opsCount, PAYMENT_SUCCESS);
    }

    result.result.results().resize(static_cast<uint32_t>(ops.size()));
    for (size_t i = 0; i < ops.size(); i++)
    {
        auto& r = result.result.results()[i];
        auto& o = ops[i];
        r.code(o.code);
        if (o.code == opINNER)
        {
            r.tr().type(o.type);
            switch (o.type)
            {
            case CREATE_ACCOUNT:
                r.tr().createAccountResult().code(o.createAccountCode);
                break;
            case PAYMENT:
                r.tr().paymentResult().code(o.paymentCode);
                break;
            case ACCOUNT_MERGE:
                r.tr().accountMergeResult().code(o.accountMergeCode);
                break;
            case SET_OPTIONS:
                r.tr().setOptionsResult().code(o.setOptionsResultCode);
                break;
            default:
                break;
            }
        }
    }

    return result;
}

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

struct ValidationResult
{
    int64_t fee;
    TransactionResultCode code;
};

TEST_CASE("txresults", "[tx][txresults]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto validate = [&](TransactionFramePtr const& tx,
                        ValidationResult validationResult,
                        TransactionResult const& applyResult = {}) {
        LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader(),
                          app->getDatabase());

        auto shouldValidateOk = validationResult.code == txSUCCESS;
        REQUIRE(tx->checkValid(*app, 0) == shouldValidateOk);
        REQUIRE(tx->getResult().result.code() == validationResult.code);
        REQUIRE(tx->getResult().feeCharged == validationResult.fee);

        // do not try to apply if checkValid returned false
        if (!shouldValidateOk)
        {
            REQUIRE(applyResult == TransactionResult{});
            return;
        }

        switch (applyResult.result.code())
        {
        case txINTERNAL_ERROR:
        case txBAD_AUTH_EXTRA:
        case txBAD_SEQ:
            return;
        default:
            break;
        }

        auto shouldApplyOk = applyResult.result.code() == txSUCCESS;
        auto applyOk = tx->apply(delta, *app);
        REQUIRE(tx->getResult() == applyResult);
        REQUIRE(applyOk == shouldApplyOk);
    };

    auto& lm = app->getLedgerManager();
    auto& clh = lm.getCurrentLedgerHeader();
    clh.scpValue.closeTime = 10;
    const int64_t baseReserve = clh.baseReserve;
    const int64_t baseFee = clh.baseFee;
    const int64_t startAmount = baseReserve * 100;
    const int64_t paymentAmount = baseReserve * 10;

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
            tx->addSignature(a);
            return;
        case Signed::DOUBLE_SIGNED:
            tx->addSignature(a);
            tx->addSignature(a);
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
                return o.code != opINNER || o.paymentCode != PAYMENT_SUCCESS;
            });
        return expectedResult(fee, opPayment.size(),
                              anyFail ? txFAILED : validationResult.code,
                              opResults);
    };

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto a = root.create("a", startAmount);
    auto b = root.create("b", startAmount);
    auto c = root.create("c", startAmount);
    auto d = root.create("d", startAmount);
    auto e = root.create("e", startAmount);
    auto f = TestAccount{*app, getAccount("f")};
    auto g = root.create("g", lm.getMinBalance(0));

    SECTION("transaction errors")
    {
        SECTION("signed")
        {
            SECTION("missing operation")
            {
                auto tx = a.tx({});
                for_all_versions(*app, [&] {
                    validate(tx, {0, txMISSING_OPERATION});
                });
            }

            SECTION("too early")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().tx.timeBounds.activate().minTime =
                    clh.scpValue.closeTime + 1;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txTOO_EARLY});
                });
            }

            SECTION("too late")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().tx.timeBounds.activate().maxTime =
                    clh.scpValue.closeTime - 1;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txTOO_LATE});
                });
            }

            SECTION("insufficent fee")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().tx.fee--;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee - 1, txINSUFFICIENT_FEE});
                });
            }

            SECTION("no account")
            {
                auto tx = f.tx({payment(root, 1)});
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txNO_ACCOUNT});
                });
            }

            SECTION("bad seq")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().tx.seqNum++;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txBAD_SEQ});
                });
            }

            SECTION("insufficent balance")
            {
                auto tx = g.tx({payment(root, 1)});
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txINSUFFICIENT_BALANCE});
                });
            }
        }

        SECTION("not signed")
        {
            SECTION("missing operation")
            {
                auto tx = a.tx({});
                tx->getEnvelope().signatures.clear();
                for_all_versions(*app, [&] {
                    validate(tx, {0, txMISSING_OPERATION});
                });
            }

            SECTION("too early")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().signatures.clear();
                tx->getEnvelope().tx.timeBounds.activate().minTime =
                    clh.scpValue.closeTime + 1;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txTOO_EARLY});
                });
            }

            SECTION("too late")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().signatures.clear();
                tx->getEnvelope().tx.timeBounds.activate().maxTime =
                    clh.scpValue.closeTime - 1;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txTOO_LATE});
                });
            }

            SECTION("insufficent fee")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().signatures.clear();
                tx->getEnvelope().tx.fee--;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee - 1, txINSUFFICIENT_FEE});
                });
            }

            SECTION("no account")
            {
                auto tx = f.tx({payment(root, 1)});
                tx->getEnvelope().signatures.clear();
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txNO_ACCOUNT});
                });
            }

            SECTION("bad seq")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->getEnvelope().signatures.clear();
                tx->getEnvelope().tx.seqNum++;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txBAD_SEQ});
                });
            }

            SECTION("insufficent balance")
            {
                auto tx = g.tx({payment(root, 1)});
                tx->getEnvelope().signatures.clear();
                for_versions_to(6, *app, [&] {
                    validate(tx, {baseFee, txBAD_AUTH});
                });
                for_versions({7}, *app, [&] {
                    validate(tx, {baseFee, txINSUFFICIENT_BALANCE});
                });
                for_versions_from(8, *app, [&] {
                    validate(tx, {baseFee, txBAD_AUTH});
                });
            }
        }

        SECTION("double signed")
        {
            SECTION("missing operation")
            {
                auto tx = a.tx({});
                tx->addSignature(a);
                for_all_versions(*app, [&] {
                    validate(tx, {0, txMISSING_OPERATION});
                });
            }

            SECTION("too early")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->addSignature(a);
                tx->getEnvelope().tx.timeBounds.activate().minTime =
                    clh.scpValue.closeTime + 1;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txTOO_EARLY});
                });
            }

            SECTION("too late")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->addSignature(a);
                tx->getEnvelope().tx.timeBounds.activate().maxTime =
                    clh.scpValue.closeTime - 1;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txTOO_LATE});
                });
            }

            SECTION("insufficent fee")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->addSignature(a);
                tx->getEnvelope().tx.fee--;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee - 1, txINSUFFICIENT_FEE});
                });
            }

            SECTION("no account")
            {
                auto tx = f.tx({payment(root, 1)});
                tx->addSignature(a);
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txNO_ACCOUNT});
                });
            }

            SECTION("bad seq")
            {
                auto tx = a.tx({payment(root, 1)});
                tx->addSignature(a);
                tx->getEnvelope().tx.seqNum++;
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txBAD_SEQ});
                });
            }

            SECTION("insufficent balance")
            {
                auto tx = g.tx({payment(root, 1)});
                tx->addSignature(a);
                for_all_versions(*app, [&] {
                    validate(tx, {baseFee, txINSUFFICIENT_BALANCE});
                });
            }
        }
    }

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

        auto tx = a.tx(operations);
        tx->getEnvelope().signatures.clear();

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
            auto validationResult = makeValidationResult(
                signs, ops, app->getLedgerManager().getCurrentLedgerVersion());
            auto applyResult = makeApplyResult(
                signs, ops, app->getLedgerManager().getCurrentLedgerVersion());
            validate(tx, validationResult, applyResult);
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

            auto applyResult =
                expectedResult(baseFee * 2, 2, txSUCCESS,
                               {PAYMENT_SUCCESS, ACCOUNT_MERGE_SUCCESS});
            applyResult.result.results()[1]
                .tr()
                .accountMergeResult()
                .sourceAccountBalance() = startAmount - 1000;
            for_all_versions(*app, [&] {
                validate(tx, {baseFee * 2, txSUCCESS}, applyResult);
            });
        }

        SECTION("with operation after")
        {
            auto tx =
                a.tx({payment(b, 1000), accountMerge(root), payment(c, 1000)});

            for_all_versions(*app, [&] {
                validate(tx, {baseFee * 3, txSUCCESS},
                         expectedResult(baseFee * 3, 3, txINTERNAL_ERROR));
            });
        }
    }

    SECTION("create account")
    {
        SECTION("normal")
        {
            auto tx = root.tx({createAccount(f, startAmount)});

            for_all_versions(*app, [&] {
                validate(tx, {baseFee * 1, txSUCCESS},
                         expectedResult(baseFee * 1, 1, txSUCCESS,
                                        {CREATE_ACCOUNT_SUCCESS}));
            });
        }

        SECTION("with payment after")
        {
            auto tx = root.tx({createAccount(f, startAmount),
                               a.op(payment(root, startAmount / 2))});
            tx->addSignature(a);

            for_all_versions(*app, [&] {
                validate(
                    tx, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {CREATE_ACCOUNT_SUCCESS, PAYMENT_SUCCESS}));
            });
        }
    }

    SECTION("not enough signature weight")
    {
        auto th = ThresholdSetter{};
        th.masterWeight = make_optional<int>(10);
        th.lowThreshold = make_optional<int>(10);
        th.medThreshold = make_optional<int>(50);
        th.highThreshold = make_optional<int>(100);

        SECTION("normal")
        {
            auto tx =
                a.tx({payment(b, 1000), setOptions(nullptr, nullptr, nullptr,
                                                   &th, nullptr, nullptr)});
            for_all_versions(*app, [&] {
                validate(
                    tx, {baseFee * 2, txSUCCESS},
                    expectedResult(baseFee * 2, 2, txSUCCESS,
                                   {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS}));
            });
        }

        SECTION("with operation after")
        {
            auto tx = a.tx(
                {payment(b, 1000),
                 setOptions(nullptr, nullptr, nullptr, &th, nullptr, nullptr),
                 payment(c, 1000)});

            for_versions_to(6, *app, [&] {
                validate(tx, {baseFee * 3, txSUCCESS},
                         expectedResult(baseFee * 3, 3, txFAILED,
                                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS,
                                         opBAD_AUTH}));
            });
            for_versions({7}, *app, [&] {
                validate(tx, {baseFee * 3, txSUCCESS},
                         expectedResult(baseFee * 3, 3, txSUCCESS,
                                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS,
                                         PAYMENT_SUCCESS}));
            });
            for_versions_from(8, *app, [&] {
                validate(tx, {baseFee * 3, txSUCCESS},
                         expectedResult(baseFee * 3, 3, txFAILED,
                                        {PAYMENT_SUCCESS, SET_OPTIONS_SUCCESS,
                                         opBAD_AUTH}));
            });
        }

        SECTION("before tx")
        {
            a.setOptions(nullptr, nullptr, nullptr, &th, nullptr, nullptr);
            auto tx = a.tx({payment(b, 1000)});

            for_versions_to(6, *app, [&] {
                validate(tx, {baseFee * 1, txFAILED});
            });
            for_versions({7}, *app, [&] {
                validate(tx, {baseFee * 1, txSUCCESS},
                         expectedResult(baseFee * 1, 1, txSUCCESS,
                                        {PAYMENT_SUCCESS}));
            });
            for_versions_from(8, *app, [&] {
                validate(tx, {baseFee * 1, txFAILED});
            });
        }
    }
}
