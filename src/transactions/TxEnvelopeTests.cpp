// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"
#include "TxTests.h"
#include "ledger/LedgerMaster.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentFrame.h"
#include "transactions/CreateOfferFrame.h"

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
    LOG(INFO) << "************ Starting envelope test";

    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application &app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    int rootSeq = 1;

    const uint64_t paymentAmount = app.getLedgerMaster().getCurrentLedgerHeader().baseReserve*10;
    
    SECTION("outer envelope")
    {
        TransactionFramePtr txFrame;
        LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

        SECTION("no signature")
        {
            txFrame = createPaymentTx(root, a1, rootSeq++, paymentAmount);
            txFrame->getEnvelope().signatures.clear();

            txFrame->apply(delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature")
        {
            txFrame = createPaymentTx(root, a1, rootSeq++, paymentAmount);
            {
                uint512 badSig;
                badSig.fill(123);
                txFrame->getEnvelope().signatures[0] = badSig;
            }

            txFrame->apply(delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
    }

    SECTION("multisig")
    {
        applyPaymentTx(app, root, a1, rootSeq++, paymentAmount);
        uint32_t a1Seq = 1;

        SecretKey s1 = getAccount("S1");
        Signer sk1(s1.getPublicKey(), 5); // below low rights

        Thresholds th;

        th[0] = 100; // weight of master key
        th[1] = 10;
        th[2] = 50;
        th[3] = 100;

        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr,
            &th, &sk1, a1Seq++);

        SecretKey s2 = getAccount("S2");
        Signer sk2(s2.getPublicKey(), 95); // med rights account

        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr,
            nullptr, &sk2, a1Seq++);

        SECTION("not enough rights (envelope)")
        {
            TransactionFramePtr tx = createPaymentTx(a1, root, a1Seq++, 1000);

            // only sign with s1
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);

            LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

            tx->apply(delta, app);
            REQUIRE(tx->getResultCode() == txBAD_AUTH);
        }

        SECTION("not enough rights (operation)")
        {
            // updating thresholds requires high
            TransactionFramePtr tx = createSetOptions(a1, nullptr,
                nullptr, nullptr, nullptr, &th, &sk1, a1Seq);

            // only sign with s1 (med)
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s2);

            LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

            tx->apply(delta, app);
            REQUIRE(tx->getResultCode() == txFAILED);
            REQUIRE(getFirstResultCode(*tx) == opBAD_AUTH);
        }

        SECTION("success two signatures")
        {
            TransactionFramePtr tx = createPaymentTx(a1, root, a1Seq++, 1000);

            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->addSignature(s2);

            LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

            tx->apply(delta, app);
            REQUIRE(tx->getResultCode() == txSUCCESS);
            REQUIRE(Payment::getInnerCode(getFirstResult(*tx)) == Payment::SUCCESS);
        }
    }

    SECTION("batching")
    {
        SECTION("empty batch")
        {
            TransactionEnvelope te;
            te.tx.account = root.getPublicKey();
            te.tx.maxFee = 1000;
            te.tx.maxLedger = 1000;
            te.tx.minLedger = 0;
            te.tx.seqNum = rootSeq++;
            TransactionFrame tx(te);
            tx.addSignature(root);
            LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

            REQUIRE(!tx.checkValid(app));

            tx.apply(delta, app);
            REQUIRE(tx.getResultCode() == txMALFORMED);
        }

        SECTION("non empty")
        {
            SecretKey b1 = getAccount("B");
            applyPaymentTx(app, root, a1, rootSeq++, paymentAmount);
            applyPaymentTx(app, root, b1, rootSeq++, paymentAmount);

            uint32_t a1Seq = 1;
            uint32_t b1Seq = 1;

            SECTION("single tx wrapped by different account")
            {
                TransactionFramePtr tx = createPaymentTx(a1, root, a1Seq++, 1000);

                // change inner payment to be b->root
                tx->getEnvelope().tx.operations[0].sourceAccount.activate() = b1.getPublicKey();

                tx->getEnvelope().signatures.clear();
                tx->addSignature(a1);

                SECTION("missing signature")
                {
                    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

                    REQUIRE(!tx->checkValid(app));
                    tx->apply(delta, app);
                    REQUIRE(tx->getResultCode() == txFAILED);
                    REQUIRE(tx->getOperations()[0]->getResultCode() == opBAD_AUTH);
                }

                SECTION("success")
                {
                    tx->addSignature(b1);
                    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

                    REQUIRE(tx->checkValid(app));
                    tx->apply(delta, app);
                    REQUIRE(tx->getResultCode() == txSUCCESS);
                    REQUIRE(Payment::getInnerCode(getFirstResult(*tx)) == Payment::SUCCESS);
                }
            }
            SECTION("multiple tx")
            {
                TransactionFramePtr tx_a = createPaymentTx(a1, root, a1Seq++, 1000);
                SECTION("one invalid tx")
                {
                    Currency idrCur = makeCurrency(b1, "IDR");
                    Price price(1, 1);
                    TransactionFramePtr tx_b = createOfferTx(b1, idrCur, idrCur, price, 1000, b1Seq);

                    // build a new tx based off tx_a and tx_b
                    tx_a->getEnvelope().tx.operations.push_back(tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.maxFee *= 2;
                    TransactionFramePtr tx = TransactionFrame::makeTransactionFromWire(tx_a->getEnvelope());

                    tx->getEnvelope().signatures.clear();
                    tx->addSignature(a1);
                    tx->addSignature(b1);

                    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

                    REQUIRE(!tx->checkValid(app));

                    tx->apply(delta, app);

                    REQUIRE(tx->getResult().feeCharged == 2 * app.getLedgerGateway().getTxFee());
                    REQUIRE(tx->getResultCode() == txFAILED);
                    // first operation was success
                    REQUIRE(Payment::getInnerCode(getFirstResult(*tx)) == Payment::SUCCESS);
                    // second 
                    REQUIRE(CreateOffer::getInnerCode(tx->getOperations()[1]->getResult()) ==
                        CreateOffer::MALFORMED);
                }
                SECTION("one failed tx")
                {
                    // this payment is too large
                    TransactionFramePtr tx_b = createPaymentTx(b1, root, b1Seq++, paymentAmount);

                    tx_a->getEnvelope().tx.operations.push_back(tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.maxFee *= 2; 
                    TransactionFramePtr tx = TransactionFrame::makeTransactionFromWire(tx_a->getEnvelope());

                    tx->getEnvelope().signatures.clear();
                    tx->addSignature(a1);
                    tx->addSignature(b1);

                    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

                    REQUIRE(tx->checkValid(app));

                    tx->apply(delta, app);

                    REQUIRE(tx->getResult().feeCharged == 2 * app.getLedgerGateway().getTxFee());
                    REQUIRE(tx->getResultCode() == txFAILED);
                    // first operation was success
                    REQUIRE(Payment::getInnerCode(getFirstResult(*tx)) == Payment::SUCCESS);
                    // second 
                    REQUIRE(Payment::getInnerCode(tx->getOperations()[1]->getResult()) ==
                        Payment::UNDERFUNDED);

                }
                SECTION("both success")
                {
                    TransactionFramePtr tx_b = createPaymentTx(b1, root, b1Seq++, 1000);

                    tx_a->getEnvelope().tx.operations.push_back(tx_b->getEnvelope().tx.operations[0]);
                    tx_a->getEnvelope().tx.maxFee *= 2; 
                    TransactionFramePtr tx = TransactionFrame::makeTransactionFromWire(tx_a->getEnvelope());

                    tx->getEnvelope().signatures.clear();
                    tx->addSignature(a1);
                    tx->addSignature(b1);

                    LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());

                    REQUIRE(tx->checkValid(app));

                    tx->apply(delta, app);

                    REQUIRE(tx->getResult().feeCharged == 2 * app.getLedgerGateway().getTxFee());
                    REQUIRE(tx->getResultCode() == txSUCCESS);

                    REQUIRE(Payment::getInnerCode(getFirstResult(*tx)) == Payment::SUCCESS);
                    REQUIRE(Payment::getInnerCode(tx->getOperations()[1]->getResult()) ==
                        Payment::SUCCESS);
                }
            }
        }
    }

    SECTION("common transaction")
    {
        TxSetFramePtr txSet = std::make_shared<TxSetFrame>();

        TransactionFramePtr txFrame = createPaymentTx(root, a1, rootSeq++, paymentAmount);
        txSet->add(txFrame);

        // close this ledger
        LedgerCloseData ledgerData(1, txSet, 1, 10);
        app.getLedgerMaster().closeLedger(ledgerData);

        REQUIRE(app.getLedgerGateway().getLedgerNum() == 3);

        {
            LedgerDelta delta(app.getLedgerMaster().getCurrentLedgerHeader());
            
            SECTION("Insufficient fee")
            {
                txFrame = createPaymentTx(root, a1, rootSeq++, paymentAmount);
                txFrame->getEnvelope().tx.maxFee = static_cast<uint32_t>(app.getLedgerMaster().getTxFee() - 1);

                txFrame->apply(delta, app);

                REQUIRE(txFrame->getResultCode() == txINSUFFICIENT_FEE);
            }
            
            SECTION("duplicate payment")
            {

                txFrame->apply(delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }

            SECTION("transaction gap")
            {
                txFrame = createPaymentTx(root, a1, rootSeq+1, paymentAmount);

                txFrame->apply(delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }

            SECTION("min ledger seq")
            {
                txFrame = createPaymentTx(root, a1, rootSeq++, paymentAmount);
                txFrame->getEnvelope().tx.minLedger = 4;
                
                txFrame->apply(delta, app);
                
                REQUIRE(txFrame->getResultCode() == txBAD_LEDGER);
            }
            
            SECTION("max ledger seq")
            {
                txFrame = createPaymentTx(root, a1, rootSeq++, paymentAmount);
                txFrame->getEnvelope().tx.maxLedger = 2;
                
                txFrame->apply(delta, app);
                
                REQUIRE(txFrame->getResultCode() == txBAD_LEDGER);
            }
        }
    }

    LOG(INFO) << "************ Ending envelope test";
}




