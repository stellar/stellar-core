// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/TxTests.h"

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
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    Hash const& networkID = app.getNetworkID();
    app.start();

    // set up world
    SecretKey root = getRoot(networkID);
    SecretKey a1 = getAccount("A");
    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    ;

    const uint64_t paymentAmount =
        app.getLedgerManager().getCurrentLedgerHeader().baseReserve * 10;

    SECTION("outer envelope")
    {
        TransactionFramePtr txFrame;
        LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                          app.getDatabase());

        SECTION("no signature")
        {
            txFrame = createCreateAccountTx(networkID, root, a1, rootSeq++,
                                            paymentAmount);
            txFrame->getEnvelope().signatures.clear();

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature")
        {
            txFrame = createCreateAccountTx(networkID, root, a1, rootSeq++,
                                            paymentAmount);
            txFrame->getEnvelope().signatures[0].signature = Signature(32, 123);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature (wrong hint)")
        {
            txFrame = createCreateAccountTx(networkID, root, a1, rootSeq++,
                                            paymentAmount);
            txFrame->getEnvelope().signatures[0].hint.fill(1);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("too many signatures (signed twice)")
        {
            txFrame = createCreateAccountTx(networkID, root, a1, rootSeq++,
                                            paymentAmount);
            txFrame->addSignature(a1);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
        }
        SECTION("too many signatures (unused signature)")
        {
            txFrame = createCreateAccountTx(networkID, root, a1, rootSeq++,
                                            paymentAmount);
            SecretKey bogus = getAccount("bogus");
            txFrame->addSignature(bogus);

            applyCheck(txFrame, delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH_EXTRA);
        }
    }

    SECTION("multisig")
    {
        applyCreateAccountTx(app, root, a1, rootSeq++, paymentAmount);
        SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

        SecretKey s1 = getAccount("S1");
        Signer sk1(s1.getPublicKey(), 5); // below low rights

        ThresholdSetter th;

        th.masterWeight = make_optional<uint8_t>(100);
        th.lowThreshold = make_optional<uint8_t>(10);
        th.medThreshold = make_optional<uint8_t>(50);
        th.highThreshold = make_optional<uint8_t>(100);

        applySetOptions(app, a1, a1Seq++, nullptr, nullptr, nullptr, &th, &sk1);

        SecretKey s2 = getAccount("S2");
        Signer sk2(s2.getPublicKey(), 95); // med rights account

        applySetOptions(app, a1, a1Seq++, nullptr, nullptr, nullptr, nullptr,
                        &sk2);

        SECTION("not enough rights (envelope)")
        {
            TransactionFramePtr tx =
                createPaymentTx(networkID, a1, root, a1Seq++, 1000);

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
                networkID, a1, a1Seq, nullptr, nullptr, nullptr, &th, &sk1);

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
            TransactionFramePtr tx =
                createPaymentTx(networkID, a1, root, a1Seq++, 1000);

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
    }

    SECTION("batching")
    {
        SECTION("empty batch")
        {
            TransactionEnvelope te;
            te.tx.sourceAccount = root.getPublicKey();
            te.tx.fee = 1000;
            te.tx.seqNum = rootSeq++;
            TransactionFramePtr tx =
                std::make_shared<TransactionFrame>(networkID, te);
            tx->addSignature(root);
            LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                              app.getDatabase());

            REQUIRE(!tx->checkValid(app, 0));

            applyCheck(tx, delta, app);
            REQUIRE(tx->getResultCode() == txMISSING_OPERATION);
        }

        SECTION("non empty")
        {
            SecretKey b1 = getAccount("B");
            applyCreateAccountTx(app, root, a1, rootSeq++, paymentAmount);
            applyCreateAccountTx(app, root, b1, rootSeq++, paymentAmount);

            SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;
            SequenceNumber b1Seq = getAccountSeqNum(b1, app) + 1;

            SECTION("single tx wrapped by different account")
            {
                TransactionFramePtr tx =
                    createPaymentTx(networkID, a1, root, a1Seq++, 1000);

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
                    createPaymentTx(networkID, a1, root, a1Seq++, 1000);
                SECTION("one invalid tx")
                {
                    Asset idrCur = makeAsset(b1, "IDR");
                    Price price(1, 1);
                    TransactionFramePtr tx_b = manageOfferOp(
                        networkID, 0, b1, idrCur, idrCur, price, 1000, b1Seq);

                    // build a new tx based off tx_a and tx_b
                    tx_b->getEnvelope()
                        .tx.operations[0]
                        .sourceAccount.activate() = b1.getPublicKey();
                    tx_a->getEnvelope().tx.operations.push_back(
                        tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.fee *= 2;
                    TransactionFramePtr tx =
                        TransactionFrame::makeTransactionFromWire(
                            networkID, tx_a->getEnvelope());

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
                    TransactionFramePtr tx_b = createPaymentTx(
                        networkID, b1, root, b1Seq++, paymentAmount);

                    tx_b->getEnvelope()
                        .tx.operations[0]
                        .sourceAccount.activate() = b1.getPublicKey();
                    tx_a->getEnvelope().tx.operations.push_back(
                        tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.fee *= 2;
                    TransactionFramePtr tx =
                        TransactionFrame::makeTransactionFromWire(
                            networkID, tx_a->getEnvelope());

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
                        createPaymentTx(networkID, b1, root, b1Seq++, 1000);

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
                    networkID, b1, c1, b1Seq++, paymentAmount / 2);

                TransactionFramePtr tx_c =
                    createPaymentTx(networkID, c1, root, 0, 1000);

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

        txFrame = createCreateAccountTx(networkID, root, a1, rootSeq++,
                                        paymentAmount);
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
                txFrame = createPaymentTx(networkID, root, a1, rootSeq++,
                                          paymentAmount);
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

                txFrame = createPaymentTx(networkID, root, a1, rootSeq,
                                          paymentAmount);
                txFrame->getEnvelope().tx.timeBounds.activate() =
                    TimeBounds(start + 1000, start + 10000);

                closeLedgerOn(app, 3, 1, 7, 2014);
                applyCheck(txFrame, delta, app);

                REQUIRE(txFrame->getResultCode() == txTOO_EARLY);

                txFrame = createPaymentTx(networkID, root, a1, rootSeq++,
                                          paymentAmount);
                txFrame->getEnvelope().tx.timeBounds.activate() =
                    TimeBounds(1000, start + 300000);

                closeLedgerOn(app, 4, 2, 7, 2014);
                applyCheck(txFrame, delta, app);
                REQUIRE(txFrame->getResultCode() == txSUCCESS);

                txFrame = createPaymentTx(networkID, root, a1, rootSeq,
                                          paymentAmount);
                txFrame->getEnvelope().tx.timeBounds.activate() =
                    TimeBounds(1000, start);

                closeLedgerOn(app, 5, 3, 7, 2014);
                applyCheck(txFrame, delta, app);
                REQUIRE(txFrame->getResultCode() == txTOO_LATE);
            }

            SECTION("transaction gap")
            {
                txFrame = createPaymentTx(networkID, root, a1, rootSeq + 1,
                                          paymentAmount);

                applyCheck(txFrame, delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }
        }
    }
}
