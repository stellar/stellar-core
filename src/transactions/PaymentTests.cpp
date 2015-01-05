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

// *STR Payment 
// *Credit Payment
// STR -> Credit Payment
// Credit -> STR Payment
// Credit -> STR -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx]")
{
    LOG(INFO) << "************ Starting payment test";

    Config cfg;
    cfg.RUN_STANDALONE = true;
    cfg.START_NEW_NETWORK = true;
    cfg.DESIRED_BASE_FEE = 10;
    //cfg.DATABASE = "postgresql://dbmaster:-island-@localhost/hayashi";

    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");

    TransactionFramePtr txFrame = createPaymentTx(root, a1, 1, 1000);

    { // simple payment
        TxDelta delta;
        txFrame->apply(delta, app);

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txSUCCESS);
        AccountFrame a1Account, rootAccount;
        REQUIRE(app.getDatabase().loadAccount(root.getPublicKey(), rootAccount));
        REQUIRE(app.getDatabase().loadAccount(a1.getPublicKey(), a1Account));
        REQUIRE(a1Account.getBalance() == 1000);
        REQUIRE(rootAccount.getBalance() == (100000000000000 - 1000 - 10));
    }
    { // make sure 2nd time fails
        TxDelta delta;
        txFrame->apply(delta, app);

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txBADSEQ);
    }

    { // credit payment with no trust
        CurrencyIssuer ci;
        ci.issuer = root.getPublicKey();
        ci.currencyCode = root.getPublicKey();

        txFrame = createCreditPaymentTx(root, a1, ci, 2, 100);
        TxDelta delta;
        txFrame->apply(delta, app);

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txNOTRUST);
    }

    {
        txFrame = setTrust(a1, root, 1, root.getPublicKey());
        TxDelta delta;
        txFrame->apply(delta, app);

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txSUCCESS);
    }

    { // simple credit payment
        CurrencyIssuer ci;
        ci.issuer = root.getPublicKey();
        ci.currencyCode = root.getPublicKey();

        txFrame = createCreditPaymentTx(root, a1, ci, 3, 100);
        TxDelta delta;
        txFrame->apply(delta, app);

        Json::Value jsonResult;
        LedgerDelta ledgerDelta;

        delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

        LOG(INFO) << jsonResult.toStyledString();

        REQUIRE(txFrame->getResultCode() == txSUCCESS);
    }

    LOG(INFO) << "************ Ending payment test";
}




