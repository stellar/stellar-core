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

using namespace stellar;
using namespace stellar::txtest;


typedef std::unique_ptr<Application> appPtr;

/*
  Tests that are testing the common envelope used in transactions.
  Things like:
    authz/authn
    double spend

// TODO.2 test making slots and trying to make a tx on an unknown slot
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

    const uint64_t paymentAmount = app.getLedgerMaster().getCurrentLedgerHeader().baseReserve*10;

    SECTION("outer envelope")
    {
        TransactionFramePtr txFrame;
        LedgerDelta delta;

        SECTION("no signature")
        {
            txFrame = createPaymentTx(root, a1, 1, paymentAmount);
            txFrame->getEnvelope().signatures.clear();

            txFrame->apply(delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature")
        {
            txFrame = createPaymentTx(root, a1, 1, paymentAmount);
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
        applyPaymentTx(app, root, a1, 1, paymentAmount);
        uint32_t a1Seq = 1;

        SecretKey s1 = getAccount("S1");
        Signer sk1(s1.getPublicKey(), 10); // med right account

        Thresholds th;

        th[0] = 100; // weight of master key
        th[1] = 1;
        th[2] = 10;
        th[3] = 100;

        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr,
            &th, &sk1, a1Seq++);

        SecretKey s2 = getAccount("S2");
        Signer sk2(s2.getPublicKey(), 90); // med right account

        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr,
            nullptr, &sk2, a1Seq++);


        SECTION("not enough rights")
        {
            TransactionFramePtr tx = createSetOptions(a1, nullptr,
                nullptr, nullptr, nullptr, &th, &sk1, a1Seq);

            // only sign with s1
            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);

            LedgerDelta delta;

            tx->apply(delta, app);
            REQUIRE(tx->getResultCode() == txBAD_AUTH);
        }

        SECTION("success two signatures")
        {
            TransactionFramePtr tx = createPaymentTx(a1, root, a1Seq++, 1000);

            tx->getEnvelope().signatures.clear();
            tx->addSignature(s1);
            tx->addSignature(s2);

            LedgerDelta delta;

            tx->apply(delta, app);
            REQUIRE(Payment::getInnerCode(tx->getResult()) == Payment::SUCCESS);
        }

    }

    SECTION("common transaction")
    {
        TxSetFramePtr txSet = std::make_shared<TxSetFrame>();

        TransactionFramePtr txFrame = createPaymentTx(root, a1, 1, paymentAmount);
        txSet->add(txFrame);

        // close this ledger
        app.getLedgerMaster().closeLedger(txSet,1,10);

        REQUIRE(app.getLedgerGateway().getLedgerNum() == 3);

        {
            LedgerDelta delta;

            SECTION("Insufficient fee")
            {
                txFrame->getEnvelope().tx.maxFee = app.getLedgerMaster().getTxFee() - 1;

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
                txFrame = createPaymentTx(root, a1, 3, paymentAmount);

                txFrame->apply(delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }

            SECTION("min ledger seq")
            {
                txFrame = createPaymentTx(root, a1, 1, paymentAmount);
                txFrame->getEnvelope().tx.minLedger = 4;
                
                txFrame->apply(delta, app);
                
                REQUIRE(txFrame->getResultCode() == txBAD_LEDGER);
            }
            
            SECTION("max ledger seq")
            {
                txFrame = createPaymentTx(root, a1, 1, paymentAmount);
                txFrame->getEnvelope().tx.maxLedger = 2;
                
                txFrame->apply(delta, app);
                
                REQUIRE(txFrame->getResultCode() == txBAD_LEDGER);
            }
        }
    }

    LOG(INFO) << "************ Ending envelope test";
}




