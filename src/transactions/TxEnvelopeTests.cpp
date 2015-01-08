// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"
#include "TxTests.h"

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
    Application app(clock, cfg);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");

    SECTION("outer envelope")
    {
        TransactionFramePtr txFrame;
        TxDelta delta;

        SECTION("no signature")
        {
            txFrame = createPaymentTx(root, a1, 1, 1000,
                [](TransactionEnvelope &e)
            {
                e.signatures.clear();
            });

            txFrame->apply(delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
        SECTION("bad signature")
        {
            txFrame = createPaymentTx(root, a1, 1, 1000,
                [](TransactionEnvelope &e)
            {
                uint512 badSig;
                badSig.fill(123);
                e.signatures[0] = badSig;
            });

            txFrame->apply(delta, app);

            REQUIRE(txFrame->getResultCode() == txBAD_AUTH);
        }
    }

    SECTION("common transaction")
    {
        TxSetFramePtr txSet = make_shared<TxSetFrame>();

        // create an account
        TransactionFramePtr txFrame = createPaymentTx(root, a1, 1, 1000);

        txSet->add(txFrame);

        // close this ledger
        app.getLedgerMaster().closeLedger(txSet);

        REQUIRE(app.getLedgerGateway().getLedgerNum() == 1);

        {
            TxDelta delta;
            Json::Value jsonResult;
            LedgerDelta ledgerDelta;

            SECTION("duplicate payment")
            {

                txFrame->apply(delta, app);

                delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

                LOG(INFO) << jsonResult.toStyledString();

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }

            TransactionFramePtr txFrame2;
            SECTION("transaction gap")
            {
                txFrame2 = createPaymentTx(root, a1, 3, 1000);

                txFrame2->apply(delta, app);

                delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

                LOG(INFO) << jsonResult.toStyledString();

                REQUIRE(txFrame->getResultCode() == txBAD_SEQ);
            }

            SECTION("min ledger seq")
            {
                txFrame = createPaymentTx(root, a1, 1, 1000,
                    [](TransactionEnvelope &e)
                {
                    e.tx.minLedger = 2;
                });

                txFrame->apply(delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_LEDGER);
            }

            SECTION("max ledger seq")
            {
                txFrame = createPaymentTx(root, a1, 1, 1000,
                    [](TransactionEnvelope &e)
                {
                    e.tx.maxLedger = 0;
                });

                txFrame->apply(delta, app);

                REQUIRE(txFrame->getResultCode() == txBAD_LEDGER);
            }
        }
    }

    LOG(INFO) << "************ Ending envelope test";
}




