// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SignatureUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

/*
  Tests that are testing the common envelope used in transactions.
  Things like:
    authz/authn
    double spend
*/

TEST_CASE("txenvelope", "[tx][envelope]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    ApplicationEditableVersion app(clock, cfg);
    Hash const& networkID = app.getNetworkID();
    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);
    SecretKey a1 = getAccount("A");

    const uint64_t paymentAmount =
        app.getLedgerManager().getCurrentLedgerHeader().baseReserve * 10;

    SECTION("outer envelope")
    {
        TransactionFramePtr txFrame;
        LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                          app.getDatabase());

        SECTION("no signature")
        {
            txFrame =
                createCreateAccountTx(app.getNetworkID(), root, a1,
                                      root.nextSequenceNumber(), paymentAmount);
            txFrame->getEnvelope().signatures.clear();

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature")
        {
            txFrame =
                createCreateAccountTx(app.getNetworkID(), root, a1,
                                      root.nextSequenceNumber(), paymentAmount);
            txFrame->getEnvelope().signatures[0].signature = Signature(32, 123);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature (wrong hint)")
        {
            txFrame =
                createCreateAccountTx(app.getNetworkID(), root, a1,
                                      root.nextSequenceNumber(), paymentAmount);
            txFrame->getEnvelope().signatures[0].hint.fill(1);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("too many signatures (signed twice)")
        {
            txFrame =
                createCreateAccountTx(app.getNetworkID(), root, a1,
                                      root.nextSequenceNumber(), paymentAmount);
            txFrame->addSignature(a1);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
        }
        SECTION("too many signatures (unused signature)")
        {
            txFrame =
                createCreateAccountTx(app.getNetworkID(), root, a1,
                                      root.nextSequenceNumber(), paymentAmount);
            SecretKey bogus = getAccount("bogus");
            txFrame->addSignature(bogus);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
        }
    }

    SECTION("multisig")
    {
        auto a1 = root.create("A", paymentAmount);

        SecretKey s1 = getAccount("S1");
        Signer sk1(KeyUtils::convertKey<SignerKey>(s1.getPublicKey()),
                   5); // below low rights

        ThresholdSetter th;

        th.masterWeight = make_optional<uint8_t>(100);
        th.lowThreshold = make_optional<uint8_t>(10);
        th.medThreshold = make_optional<uint8_t>(50);
        th.highThreshold = make_optional<uint8_t>(100);

        a1.setOptions(nullptr, nullptr, nullptr, &th, &sk1, nullptr);

        SecretKey s2 = getAccount("S2");
        Signer sk2(KeyUtils::convertKey<SignerKey>(s2.getPublicKey()),
                   95); // med rights account

        a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

        SECTION("not enough rights (envelope)")
        {
            TransactionFramePtr tx = createPaymentTx(
                app.getNetworkID(), a1, root, a1.nextSequenceNumber(), 1000);

            // only sign with s1
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);

            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            applyCheck(tx, delta, app);
            REQUIRE(tx->getResultCode() == txBAD_AUTH);
        }

        SECTION("not enough rights (operation)")
        {
            // updating thresholds requires high
            TransactionFramePtr tx = createSetOptions(
                app.getNetworkID(), a1, a1.nextSequenceNumber(), nullptr,
                nullptr, nullptr, &th, &sk1, nullptr);

            // only sign with s1 (med)
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s2);

            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            applyCheck(tx, delta, app);
            REQUIRE(tx->getResultCode() == txFAILED);
            REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
        }

        SECTION("success two signatures")
        {
            TransactionFramePtr tx = createPaymentTx(
                app.getNetworkID(), a1, root, a1.nextSequenceNumber(), 1000);

            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->addSignature(s2);

            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            applyCheck(tx, delta, app);
            REQUIRE(tx->getResultCode() == txSUCCESS);
            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*tx)) ==
                    PAYMENT_SUCCESS);
        }

        SECTION("do not allow duplicate signature")
        {
            TransactionFramePtr tx = createPaymentTx(
                networkID, a1, root, a1.nextSequenceNumber(), 1000);

            tx->getEnvelope().signatures.clear();
            for (auto i = 0; i < 10; i++)
                tx->addSignature(s1);

            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            applyCheck(tx, delta, app);
            REQUIRE(tx->getResultCode() == txBAD_AUTH);
        }
    }

    SECTION("alternative signatures")
    {
        auto a1 = root.create("A", paymentAmount);

        struct AltSignature
        {
            std::string name;
            bool removeAfterSucces;
            std::function<SignerKey(TransactionFrame const&)> createSigner;
            std::function<void(TransactionFrame&)> sign;
        };

        // ensue that hash(x) supports 0 inside 'x'
        auto x = std::vector<uint8_t>{'a', 'b', 'c', 0,   'd', 'e', 'f', 0,
                                      0,   0,   'g', 'h', 'i', 'j', 'k', 'l',
                                      'A', 'B', 'C', 0,   'D', 'E', 'F', 0,
                                      0,   0,   'G', 'H', 'I', 'J', 'K', 'L'};
        auto alternatives = std::vector<AltSignature>{
            AltSignature{"hash tx", true,
                         [](TransactionFrame const& tx) {
                             return SignerKeyUtils::preAuthTxKey(tx);
                         },
                         [](TransactionFrame&) {}},
            AltSignature{"hash x", false,
                         [x](TransactionFrame const&) {
                             return SignerKeyUtils::hashXKey(x);
                         },
                         [x](TransactionFrame& tx) {
                             tx.addSignature(SignatureUtils::signHashX(x));
                         }}};

        for (auto const& alternative : alternatives)
        {
            SECTION(alternative.name)
            {
                SECTION("protocol version 2")
                {
                    app.getLedgerManager().setCurrentLedgerVersion(2);

                    TransactionFramePtr tx =
                        createPaymentTx(app.getNetworkID(), a1, root,
                                        a1.getLastSequenceNumber() + 2, 1000);
                    tx->getEnvelope().signatures.clear();

                    SignerKey sk = alternative.createSigner(*tx);
                    Signer sk1(sk, 1);
                    REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, nullptr,
                                                    nullptr, &sk1, nullptr),
                                      ex_SET_OPTIONS_BAD_SIGNER);
                }

                SECTION("protocol version 3")
                {
                    app.getLedgerManager().setCurrentLedgerVersion(3);

                    SECTION("single signature")
                    {
                        SECTION("invalid seq nr")
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 1, 1000);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txBAD_SEQ);
                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                        }

                        SECTION("invalid signature")
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 2, 1000);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            KeyFunctions<SignerKey>::getKeyValue(sk)[0] ^= 0x01;
                            Signer sk1(sk, 1);
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                        }

                        SECTION("too many signatures (signed by owner)")
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 2, 1000);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH_EXTRA);
                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                        }

                        SECTION("success")
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 2, 1000);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, app).size() ==
                                    (alternative.removeAfterSucces ? 0 : 1));
                        }

                        SECTION("merge signing account")
                        {
                            auto b1 = root.create("a1", paymentAmount);
                            a1.pay(b1, 1000);

                            TransactionFramePtr tx = createAccountMerge(
                                app.getNetworkID(), b1, a1,
                                b1.getLastSequenceNumber() + 2);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            b1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            REQUIRE(getAccountSigners(a1, app).size() == 0);
                            REQUIRE(getAccountSigners(b1, app).size() == 1);
                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(MergeOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == ACCOUNT_MERGE_SUCCESS);
                            REQUIRE(getAccountSigners(a1, app).size() == 0);
                            REQUIRE(!loadAccount(b1, app, false));
                        }

                        SECTION("failing transaction")
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 2, -1);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 1);
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == stellar::txFAILED);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == stellar::PAYMENT_MALFORMED);
                            REQUIRE(getAccountSigners(a1, app).size() == 1);
                        }
                    }

                    SECTION("multisig")
                    {
                        SecretKey s1 = getAccount("S1");
                        Signer sk1(
                            KeyUtils::convertKey<SignerKey>(s1.getPublicKey()),
                            95);

                        ThresholdSetter th;

                        th.masterWeight = make_optional<uint8_t>(100);
                        th.lowThreshold = make_optional<uint8_t>(10);
                        th.medThreshold = make_optional<uint8_t>(50);
                        th.highThreshold = make_optional<uint8_t>(100);

                        a1.setOptions(nullptr, nullptr, nullptr, &th, &sk1,
                                      nullptr);

                        SECTION("not enough rights (envelope)")
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 2, 1000);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            REQUIRE(getAccountSigners(a1, app).size() == 2);
                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, app).size() == 2);
                        }

                        SECTION("not enough rights (operation)")
                        {
                            // updating thresholds requires high
                            TransactionFramePtr tx = createSetOptions(
                                app.getNetworkID(), a1,
                                a1.getLastSequenceNumber() + 2, nullptr,
                                nullptr, nullptr, &th, nullptr, nullptr);
                            tx->getEnvelope().signatures.clear();

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 95); // med rights account
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            REQUIRE(getAccountSigners(a1, app).size() == 2);
                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txFAILED);
                            REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
                            REQUIRE(getAccountSigners(a1, app).size() == 2);
                        }

                        SECTION("success signature + " + alternative.name)
                        {
                            TransactionFramePtr tx = createPaymentTx(
                                app.getNetworkID(), a1, root,
                                a1.getLastSequenceNumber() + 2, 1000);

                            tx->getEnvelope().signatures.clear();
                            tx->addSignature(s1);

                            SignerKey sk = alternative.createSigner(*tx);
                            Signer sk1(sk, 5); // below low rights
                            a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                          &sk1, nullptr);

                            REQUIRE(getAccountSigners(a1, app).size() == 2);
                            LedgerDelta delta(
                                app.getLedgerManager().getCurrentLedgerHeader(),
                                app.getDatabase());

                            alternative.sign(*tx);
                            applyCheck(tx, delta, app);
                            REQUIRE(tx->getResultCode() == txSUCCESS);
                            REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(
                                        *tx)) == PAYMENT_SUCCESS);
                            REQUIRE(getAccountSigners(a1, app).size() ==
                                    (alternative.removeAfterSucces ? 1 : 2));
                        }
                    }

                    SECTION(alternative.name + " in op source account signers")
                    {
                        Operation op =
                            createPaymentOp(&a1.getSecretKey(), root, 100);
                        TransactionFramePtr tx = transactionFromOperation(
                            app.getNetworkID(), root,
                            root.getLastSequenceNumber() + 2, op);
                        tx->getEnvelope().signatures.clear();

                        SignerKey sk = alternative.createSigner(*tx);
                        Signer sk1(sk, 1);
                        root.setOptions(nullptr, nullptr, nullptr, nullptr,
                                        &sk1, nullptr);
                        a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk1,
                                      nullptr);

                        LedgerDelta delta(
                            app.getLedgerManager().getCurrentLedgerHeader(),
                            app.getDatabase());

                        REQUIRE(getAccountSigners(root, app).size() == 1);
                        REQUIRE(getAccountSigners(a1, app).size() == 1);
                        alternative.sign(*tx);
                        applyCheck(tx, delta, app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(getAccountSigners(root, app).size() ==
                                (alternative.removeAfterSucces ? 0 : 1));
                        REQUIRE(getAccountSigners(a1, app).size() ==
                                (alternative.removeAfterSucces ? 0 : 1));
                    }

                    SECTION(alternative.name +
                            " in multiple ops source account signers")
                    {
                        Operation op =
                            createPaymentOp(&a1.getSecretKey(), root, 100);
                        TransactionFramePtr tx = transactionFromOperations(
                            app.getNetworkID(), root,
                            root.getLastSequenceNumber() + 2, {op, op});
                        tx->getEnvelope().signatures.clear();

                        SignerKey sk = alternative.createSigner(*tx);
                        Signer sk1(sk, 1);
                        root.setOptions(nullptr, nullptr, nullptr, nullptr,
                                        &sk1, nullptr);
                        a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk1,
                                      nullptr);

                        LedgerDelta delta(
                            app.getLedgerManager().getCurrentLedgerHeader(),
                            app.getDatabase());

                        REQUIRE(getAccountSigners(root, app).size() == 1);
                        REQUIRE(getAccountSigners(a1, app).size() == 1);
                        alternative.sign(*tx);
                        applyCheck(tx, delta, app);
                        REQUIRE(tx->getResultCode() == txSUCCESS);
                        REQUIRE(PaymentOpFrame::getInnerCode(
                                    getFirstResult(*tx)) == PAYMENT_SUCCESS);
                        REQUIRE(getAccountSigners(root, app).size() ==
                                (alternative.removeAfterSucces ? 0 : 1));
                        REQUIRE(getAccountSigners(a1, app).size() ==
                                (alternative.removeAfterSucces ? 0 : 1));
                    }
                }
            }
        }
    }

    SECTION("batching")
    {
        SECTION("empty batch")
        {
            TransactionEnvelope te;
            te.tx.sourceAccount = root.getPublicKey();
            te.tx.fee = 1000;
            te.tx.seqNum = root.nextSequenceNumber();
            TransactionFramePtr tx =
                std::make_shared<TransactionFrame>(app.getNetworkID(), te);
            tx->addSignature(root);
            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            REQUIRE(!tx->checkValid(app, 0));

            applyCheck(tx, delta, app);
            REQUIRE(tx->getResultCode() == txMISSING_OPERATION);
        }

        SECTION("non empty")
        {
            auto a1 = root.create("A", paymentAmount);
            auto b1 = root.create("B", paymentAmount);

            SECTION("single tx wrapped by different account")
            {
                TransactionFramePtr tx =
                    createPaymentTx(app.getNetworkID(), a1, root,
                                    a1.nextSequenceNumber(), 1000);

                // change inner payment to be b->root
                tx->getEnvelope().tx.operations[0].sourceAccount.activate() =
                    b1.getPublicKey();

                tx->getEnvelope().signatures.clear();
                tx->addSignature(a1);

                SECTION("missing signature")
                {
                    LedgerDelta delta(
                        app.getLedgerManager().getCurrentLedgerHeader(),
                        app.getDatabase());

                    REQUIRE(!tx->checkValid(app, 0));
                    applyCheck(tx, delta, app);
                    REQUIRE(tx->getResultCode() == txFAILED);
                    REQUIRE(tx->getOperations()[0]->getResultCode() ==
                            opBAD_AUTH);
                }

                SECTION("success")
                {
                    tx->addSignature(b1);
                    LedgerDelta delta(
                        app.getLedgerManager().getCurrentLedgerHeader(),
                        app.getDatabase());

                    REQUIRE(tx->checkValid(app, 0));
                    applyCheck(tx, delta, app);
                    REQUIRE(tx->getResultCode() == txSUCCESS);
                    REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*tx)) ==
                            PAYMENT_SUCCESS);
                }
            }
            SECTION("multiple tx")
            {
                TransactionFramePtr tx_a =
                    createPaymentTx(app.getNetworkID(), a1, root,
                                    a1.nextSequenceNumber(), 1000);
                SECTION("one invalid tx")
                {
                    Asset idrCur = makeAsset(b1, "IDR");
                    Price price(1, 1);
                    TransactionFramePtr tx_b =
                        manageOfferOp(app.getNetworkID(), 0, b1, idrCur, idrCur,
                                      price, 1000, b1.getLastSequenceNumber());

                    // build a new tx based off tx_a and tx_b
                    tx_b->getEnvelope()
                        .tx.operations[0]
                        .sourceAccount.activate() = b1.getPublicKey();
                    tx_a->getEnvelope().tx.operations.push_back(
                        tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.fee *= 2;
                    TransactionFramePtr tx =
                        TransactionFrame::makeTransactionFromWire(
                            app.getNetworkID(), tx_a->getEnvelope());

                    tx->getEnvelope().signatures.clear();
                    tx->addSignature(a1);
                    tx->addSignature(b1);

                    LedgerDelta delta(
                        app.getLedgerManager().getCurrentLedgerHeader(),
                        app.getDatabase());

                    REQUIRE(!tx->checkValid(app, 0));

                    applyCheck(tx, delta, app);

                    REQUIRE(tx->getResult().feeCharged ==
                            2 * app.getLedgerManager().getTxFee());
                    REQUIRE(tx->getResultCode() == txFAILED);
                    // first operation was success
                    REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*tx)) ==
                            PAYMENT_SUCCESS);
                    // second
                    REQUIRE(ManageOfferOpFrame::getInnerCode(
                                tx->getOperations()[1]->getResult()) ==
                            MANAGE_OFFER_MALFORMED);
                }
                SECTION("one failed tx")
                {
                    // this payment is too large
                    TransactionFramePtr tx_b =
                        createPaymentTx(app.getNetworkID(), b1, root,
                                        b1.nextSequenceNumber(), paymentAmount);

                    tx_b->getEnvelope()
                        .tx.operations[0]
                        .sourceAccount.activate() = b1.getPublicKey();
                    tx_a->getEnvelope().tx.operations.push_back(
                        tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.fee *= 2;
                    TransactionFramePtr tx =
                        TransactionFrame::makeTransactionFromWire(
                            app.getNetworkID(), tx_a->getEnvelope());

                    tx->getEnvelope().signatures.clear();
                    tx->addSignature(a1);
                    tx->addSignature(b1);

                    LedgerDelta delta(
                        app.getLedgerManager().getCurrentLedgerHeader(),
                        app.getDatabase());

                    REQUIRE(tx->checkValid(app, 0));

                    applyCheck(tx, delta, app);

                    REQUIRE(tx->getResult().feeCharged ==
                            2 * app.getLedgerManager().getTxFee());
                    REQUIRE(tx->getResultCode() == txFAILED);
                    // first operation was success
                    REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*tx)) ==
                            PAYMENT_SUCCESS);
                    // second
                    REQUIRE(PaymentOpFrame::getInnerCode(
                                tx->getOperations()[1]->getResult()) ==
                            PAYMENT_UNDERFUNDED);
                }
                SECTION("both success")
                {
                    TransactionFramePtr tx_b =
                        createPaymentTx(app.getNetworkID(), b1, root,
                                        b1.nextSequenceNumber(), 1000);

                    tx_b->getEnvelope()
                        .tx.operations[0]
                        .sourceAccount.activate() = b1.getPublicKey();
                    tx_a->getEnvelope().tx.operations.push_back(
                        tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.fee *= 2;
                    TransactionFramePtr tx =
                        TransactionFrame::makeTransactionFromWire(
                            app.getNetworkID(), tx_a->getEnvelope());

                    tx->getEnvelope().signatures.clear();
                    tx->addSignature(a1);
                    tx->addSignature(b1);

                    LedgerDelta delta(
                        app.getLedgerManager().getCurrentLedgerHeader(),
                        app.getDatabase());

                    REQUIRE(tx->checkValid(app, 0));

                    applyCheck(tx, delta, app);

                    REQUIRE(tx->getResult().feeCharged ==
                            2 * app.getLedgerManager().getTxFee());
                    REQUIRE(tx->getResultCode() == txSUCCESS);

                    REQUIRE(PaymentOpFrame::getInnerCode(getFirstResult(*tx)) ==
                            PAYMENT_SUCCESS);
                    REQUIRE(PaymentOpFrame::getInnerCode(
                                tx->getOperations()[1]->getResult()) ==
                            PAYMENT_SUCCESS);
                }
            }
            SECTION("operation using default signature")
            {
                SecretKey c1 = getAccount("C");

                // build a transaction:
                //  1. B funds C
                //  2. send from C -> root

                TransactionFramePtr tx = createCreateAccountTx(
                    app.getNetworkID(), b1, c1, b1.nextSequenceNumber(),
                    paymentAmount / 2);

                TransactionFramePtr tx_c =
                    createPaymentTx(app.getNetworkID(), c1, root, 0, 1000);

                tx_c->getEnvelope().tx.operations[0].sourceAccount.activate() =
                    c1.getPublicKey();

                tx->getEnvelope().tx.operations.push_back(
                    tx_c->getEnvelope().tx.operations[0]);

                tx->getEnvelope().tx.fee *= 2;

                tx->getEnvelope().signatures.clear();
                tx->addSignature(b1);
                tx->addSignature(c1);

                LedgerDelta delta(
                    app.getLedgerManager().getCurrentLedgerHeader(),
                    app.getDatabase());

                REQUIRE(tx->checkValid(app, 0));

                applyCheck(tx, delta, app);

                REQUIRE(tx->getResult().feeCharged ==
                        2 * app.getLedgerManager().getTxFee());
                REQUIRE(tx->getResultCode() == txSUCCESS);

                REQUIRE(CreateAccountOpFrame::getInnerCode(
                            getFirstResult(*tx)) == CREATE_ACCOUNT_SUCCESS);
                REQUIRE(PaymentOpFrame::getInnerCode(
                            tx->getOperations()[1]->getResult()) ==
                        PAYMENT_SUCCESS);
            }
        }
    }

    SECTION("common transaction")
    {
        TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
            app.getLedgerManager().getLastClosedLedgerHeader().hash);

        TransactionFramePtr txFrame;

        txFrame =
            createCreateAccountTx(app.getNetworkID(), root, a1,
                                  root.nextSequenceNumber(), paymentAmount);
        txSet->add(txFrame);

        // close this ledger
        StellarValue sv(txSet->getContentsHash(), 1, emptyUpgradeSteps, 0);
        LedgerCloseData ledgerData(1, txSet, sv);
        app.getLedgerManager().closeLedger(ledgerData);

        REQUIRE(app.getLedgerManager().getLedgerNum() == 3);

        {
            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            SECTION("Insufficient fee")
            {
                txFrame =
                    createPaymentTx(app.getNetworkID(), root, a1,
                                    root.nextSequenceNumber(), paymentAmount);
                txFrame->getEnvelope().tx.fee = static_cast<uint32_t>(
                    app.getLedgerManager().getTxFee() - 1);

                applyCheck(txFrame, delta, app);

                REQUIRE(txFrame->getResultCode() == txINSUFFICIENT_FEE);
            }

            SECTION("duplicate payment")
            {
                applyCheck(txFrame, delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }

            SECTION("time issues")
            {
                // tx too young
                // tx ok
                // tx too old
                VirtualClock::time_point ledgerTime;
                time_t start = getTestDate(1, 7, 2014);
                ledgerTime = VirtualClock::from_time_t(start);

                clock.setCurrentTime(ledgerTime);

                txFrame =
                    createPaymentTx(app.getNetworkID(), root, a1,
                                    root.nextSequenceNumber(), paymentAmount);
                txFrame->getEnvelope().tx.timeBounds.activate() =
                    TimeBounds(start + 1000, start + 10000);

                closeLedgerOn(app, 3, 1, 7, 2014);
                applyCheck(txFrame, delta, app);

                REQUIRE(txFrame->getResultCode() == txTOO_EARLY);

                txFrame =
                    createPaymentTx(app.getNetworkID(), root, a1,
                                    root.nextSequenceNumber(), paymentAmount);
                txFrame->getEnvelope().tx.timeBounds.activate() =
                    TimeBounds(1000, start + 300000);

                closeLedgerOn(app, 4, 2, 7, 2014);
                applyCheck(txFrame, delta, app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);

                txFrame =
                    createPaymentTx(app.getNetworkID(), root, a1,
                                    root.nextSequenceNumber(), paymentAmount);
                txFrame->getEnvelope().tx.timeBounds.activate() =
                    TimeBounds(1000, start);

                closeLedgerOn(app, 5, 3, 7, 2014);
                applyCheck(txFrame, delta, app);
                REQUIRE(txFrame->getResultCode() == txTOO_LATE);
            }

            SECTION("transaction gap")
            {
                txFrame = createPaymentTx(app.getNetworkID(), root, a1,
                                          root.getLastSequenceNumber(),
                                          paymentAmount);

                applyCheck(txFrame, delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }
        }
    }
}
