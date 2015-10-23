// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
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
    using xdr::operator==;

    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey a1 = getAccount("A");

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    applyCreateAccountTx(app, root, a1, rootSeq++,
                         app.getLedgerManager().getMinBalance(0) + 1000);

    SequenceNumber a1seq = getAccountSeqNum(a1, app) + 1;

    SECTION("Signers")
    {
        SecretKey s1 = getAccount("S1");
        Signer sk1(s1.getPublicKey(), 1); // low right account

        ThresholdSetter th;

        th.masterWeight = make_optional<uint8_t>(100);
        th.lowThreshold = make_optional<uint8_t>(1);
        th.medThreshold = make_optional<uint8_t>(10);
        th.highThreshold = make_optional<uint8_t>(100);

        SECTION("insufficient balance")
        {
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr, &th,
                            &sk1, nullptr, SET_OPTIONS_LOW_RESERVE);
        }

        SECTION("can't use master key as alternate signer")
        {
            Signer sk(a1.getPublicKey(), 100);
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                            nullptr, &sk, nullptr, SET_OPTIONS_BAD_SIGNER);
        }

        SECTION("multiple signers")
        {
            // add some funds
            applyPaymentTx(app, root, a1, rootSeq++,
                           app.getLedgerManager().getMinBalance(2));

            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr, &th,
                            &sk1, nullptr);

            AccountFrame::pointer a1Account;

            a1Account = loadAccount(a1, app);
            REQUIRE(a1Account->getAccount().signers.size() == 1);
            {
                Signer& a_sk1 = a1Account->getAccount().signers[0];
                REQUIRE(a_sk1.pubKey == sk1.pubKey);
                REQUIRE(a_sk1.weight == sk1.weight);
            }

            // add signer 2
            SecretKey s2 = getAccount("S2");
            Signer sk2(s2.getPublicKey(), 100);
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                            nullptr, &sk2, nullptr);

            a1Account = loadAccount(a1, app);
            REQUIRE(a1Account->getAccount().signers.size() == 2);

            // update signer 2
            sk2.weight = 11;
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                            nullptr, &sk2, nullptr);

            // update signer 1
            sk1.weight = 11;
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                            nullptr, &sk1, nullptr);

            sk1.weight = 0;
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                            nullptr, &sk1, nullptr);

            // remove signer 1
            a1Account = loadAccount(a1, app);
            REQUIRE(a1Account->getAccount().signers.size() == 1);
            Signer& a_sk2 = a1Account->getAccount().signers[0];
            REQUIRE(a_sk2.pubKey == sk2.pubKey);
            REQUIRE(a_sk2.weight == sk2.weight);

            // remove signer 2
            sk2.weight = 0;
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                            nullptr, &sk2, nullptr);

            a1Account = loadAccount(a1, app);
            REQUIRE(a1Account->getAccount().signers.size() == 0);
        }
    }

    SECTION("flags")
    {
        SECTION("Can't set and clear same flag")
        {
            uint32_t setFlags = AUTH_REQUIRED_FLAG;
            uint32_t clearFlags = AUTH_REQUIRED_FLAG;
            applySetOptions(app, a1, a1seq++, nullptr, &setFlags, &clearFlags,
                            nullptr, nullptr, nullptr, SET_OPTIONS_BAD_FLAGS);
        }
        SECTION("auth flags")
        {
            uint32_t flags;

            flags = AUTH_REQUIRED_FLAG;
            applySetOptions(app, a1, a1seq++, nullptr, &flags, nullptr, nullptr,
                            nullptr, nullptr);

            flags = AUTH_REVOCABLE_FLAG;
            applySetOptions(app, a1, a1seq++, nullptr, &flags, nullptr, nullptr,
                            nullptr, nullptr);

            // clear flag
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, &flags, nullptr,
                            nullptr, nullptr);

            flags = AUTH_IMMUTABLE_FLAG;
            applySetOptions(app, a1, a1seq++, nullptr, &flags, nullptr, nullptr,
                            nullptr, nullptr);

            // at this point trying to change any flag should fail

            applySetOptions(app, a1, a1seq++, nullptr, nullptr, &flags, nullptr,
                            nullptr, nullptr, SET_OPTIONS_CANT_CHANGE);

            flags = AUTH_REQUIRED_FLAG;
            applySetOptions(app, a1, a1seq++, nullptr, nullptr, &flags, nullptr,
                            nullptr, nullptr, SET_OPTIONS_CANT_CHANGE);

            flags = AUTH_REVOCABLE_FLAG;
            applySetOptions(app, a1, a1seq++, nullptr, &flags, nullptr, nullptr,
                            nullptr, nullptr, SET_OPTIONS_CANT_CHANGE);
        }
    }

    SECTION("Home domain")
    {
        SECTION("invalid home domain")
        {
            std::string bad[] = {"abc\r", "abc\x7F", std::string("ab\000c", 4)};
            for (auto& s : bad)
            {
                applySetOptions(app, a1, a1seq++, nullptr, nullptr, nullptr,
                                nullptr, nullptr, &s,
                                SET_OPTIONS_INVALID_HOME_DOMAIN);
            }
        }
    }

    // these are all tested by other tests
    // set InflationDest
    // set flags
    // set transfer rate
    // set data
    // set thresholds
    // set signer
}
