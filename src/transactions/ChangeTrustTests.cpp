// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "TxTests.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    Database& db = app.getDatabase();

    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey gateway = getAccount("gw");

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    SECTION("basic tests")
    {
        const int64_t minBalance2 = app.getLedgerManager().getMinBalance(2);

        applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
        SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

        Asset idrCur = makeAsset(gateway, "IDR");

        // create a trustline with a limit of 100
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 100);

        // fill it to 90
        applyCreditPaymentTx(app, gateway, root, idrCur, gateway_seq++, 90);

        // can't lower the limit below balance
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 89,
                         CHANGE_TRUST_INVALID_LIMIT);
        // can't delete if there is a balance
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 0,
                         CHANGE_TRUST_INVALID_LIMIT);

        // lower the limit at the balance
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 90);

        // clear the balance
        applyCreditPaymentTx(app, root, gateway, idrCur, rootSeq++, 90);
        // delete the trust line
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 0);
        REQUIRE(!(TrustFrame::loadTrustLine(root.getPublicKey(), idrCur, db)));
    }
    SECTION("issuer does not exist")
    {
        SECTION("new trust line")
        {
            applyChangeTrust(app, root, gateway, rootSeq, "USD", 100,
                             CHANGE_TRUST_NO_ISSUER);
        }
        SECTION("edit existing")
        {
            const int64_t minBalance2 = app.getLedgerManager().getMinBalance(2);

            applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
            SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

            applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 100);
            // Merge gateway back into root (the trustline still exists)
            applyAccountMerge(app, gateway, root, gateway_seq++);

            applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 99,
                             CHANGE_TRUST_NO_ISSUER);
        }
    }
}
