// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "TxTests.h"
#include "transactions/TransactionFrame.h"
#include "ledger/LedgerDelta.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Try setting each option to make sure it works
// try setting all at once
// try setting high threshold ones without the correct sigs
// make sure it doesn't allow us to add signers when we don't have the
// minbalance
TEST_CASE("set options", "[tx][setoptions]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    applyPaymentTx(app, root, a1, rootSeq++,
                   app.getLedgerManager().getMinBalance(0) + 1000);

    SequenceNumber a1seq = getAccountSeqNum(a1, app) + 1;

    SECTION("Signers")
    {
        SecretKey s1 = getAccount("S1");
        Signer sk1(s1.getPublicKey(), 1); // low right account

        Thresholds th;

        th[0] = 100; // weight of master key
        th[1] = 1;
        th[2] = 10;
        th[3] = 100;

        SECTION("insufficient balance")
        {
            applySetOptions(app, a1, nullptr, nullptr, nullptr, &th, &sk1,
                            a1seq++, SET_OPTIONS_BELOW_MIN_BALANCE);
        }

        applyPaymentTx(app, root, a1, rootSeq++,
                       app.getLedgerManager().getMinBalance(2));

        applySetOptions(app, a1, nullptr, nullptr, nullptr, &th, &sk1, a1seq++);

        AccountFrame a1Account;

        REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account,
                                          app.getDatabase(), true));
        REQUIRE(a1Account.getAccount().signers.size() == 1);
        Signer& a_sk1 = a1Account.getAccount().signers[0];
        REQUIRE(a_sk1.pubKey == sk1.pubKey);
        REQUIRE(a_sk1.weight == sk1.weight);

        // add signer 2
        SecretKey s2 = getAccount("S2");
        Signer sk2(s2.getPublicKey(), 100);
        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr, &sk2,
                        a1seq++);

        REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account,
                                          app.getDatabase(), true));
        REQUIRE(a1Account.getAccount().signers.size() == 2);

        // update signer 2
        sk2.weight = 11;
        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr, &sk2,
                        a1seq++);

        // update signer 1
        sk1.weight = 11;
        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr, &sk1,
                        a1seq++);

        // remove signer 1
        sk1.weight = 0;
        applySetOptions(app, a1, nullptr, nullptr, nullptr, nullptr, &sk1,
                        a1seq++);

        REQUIRE(AccountFrame::loadAccount(a1.getPublicKey(), a1Account,
                                          app.getDatabase(), true));
        REQUIRE(a1Account.getAccount().signers.size() == 1);
        Signer& a_sk2 = a1Account.getAccount().signers[0];
        REQUIRE(a_sk2.pubKey == sk2.pubKey);
        REQUIRE(a_sk2.weight == sk2.weight);
    }

    SECTION("Can't set and clear same flag")
    {
        uint32_t setFlags = AUTH_REQUIRED_FLAG;
        uint32_t clearFlags = AUTH_REQUIRED_FLAG;
        applySetOptions(app, a1, nullptr, &setFlags, &clearFlags, nullptr,
                        nullptr, a1seq++, SET_OPTIONS_MALFORMED);
    }

    // these are all tested by other tests
    // set InflationDest
    // set flags
    // set transfer rate
    // set data
    // set thresholds
    // set signer
}
