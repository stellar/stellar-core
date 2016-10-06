// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "transactions/TxTests.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("allow trust", "[tx][allowtrust]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto appPtr = Application::create(clock, cfg);
    auto& app = *appPtr;
    auto& db = app.getDatabase();

    app.start();

    const int64_t assetMultiplier = 10000000;
    const int64_t trustLineLimit = INT64_MAX;
    const int64_t trustLineStartingBalance = 20000 * assetMultiplier;

    // set up world
    auto root = getRoot(app.getNetworkID());
    auto gateway = getAccount("gw");
    auto a1 = getAccount("A");
    auto a2 = getAccount("2");

    auto rootSeq = getAccountSeqNum(root, app) + 1;
    auto const minBalance2 = app.getLedgerManager().getMinBalance(2);

    applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
    auto gatewaySeq = getAccountSeqNum(gateway, app) + 1;

    applyCreateAccountTx(app, root, a1, rootSeq++, minBalance2);
    auto a1Seq = getAccountSeqNum(a1, app) + 1;

    applyCreateAccountTx(app, root, a2, rootSeq++, minBalance2);
    auto a2Seq = getAccountSeqNum(a2, app) + 1;

    auto idrCur = makeAsset(gateway, "IDR");

    SECTION("allow trust not required")
    {
        applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", true, ALLOW_TRUST_TRUST_NOT_REQUIRED);
        applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", false, ALLOW_TRUST_TRUST_NOT_REQUIRED);
    }

    SECTION("allow trust without trustline")
    {
        auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
        applySetOptions(app, gateway, gatewaySeq++, nullptr, &setFlags,
                        nullptr, nullptr, nullptr, nullptr);

        SECTION("do not set revocable flag")
        {
            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", true, ALLOW_TRUST_NO_TRUST_LINE);
            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", false, ALLOW_TRUST_CANT_REVOKE);
        }
        SECTION("set revocable flag")
        {
            auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
            applySetOptions(app, gateway, gatewaySeq++, nullptr, &setFlags,
                            nullptr, nullptr, nullptr, nullptr);

            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", true, ALLOW_TRUST_NO_TRUST_LINE);
            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", false, ALLOW_TRUST_NO_TRUST_LINE);
        }
    }

    SECTION("allow trust not required with payment")
    {
        applyChangeTrust(app, a1, gateway, a1Seq++, "IDR", trustLineLimit);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gatewaySeq++,
                             trustLineStartingBalance);
        applyCreditPaymentTx(app, a1, gateway, idrCur, a1Seq++,
                             trustLineStartingBalance);
    }

    SECTION("allow trust required")
    {
        auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
        applySetOptions(app, gateway, gatewaySeq++, nullptr, &setFlags,
                        nullptr, nullptr, nullptr, nullptr);

        applyChangeTrust(app, a1, gateway, a1Seq++, "IDR", trustLineLimit);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gatewaySeq++,
                             trustLineStartingBalance, PAYMENT_NOT_AUTHORIZED);

        applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", true);
        applyCreditPaymentTx(app, gateway, a1, idrCur, gatewaySeq++,
                             trustLineStartingBalance);

        SECTION("do not set revocable flag")
        {
            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", false,
                            ALLOW_TRUST_CANT_REVOKE);
            applyCreditPaymentTx(app, a1, gateway, idrCur, a1Seq++,
                                trustLineStartingBalance);

            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", false,
                            ALLOW_TRUST_CANT_REVOKE);
        }
        SECTION("set revocable flag")
        {
            auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
            applySetOptions(app, gateway, gatewaySeq++, nullptr, &setFlags,
                            nullptr, nullptr, nullptr, nullptr);

            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", false);
            applyCreditPaymentTx(app, a1, gateway, idrCur, a1Seq++,
                                trustLineStartingBalance,
                                PAYMENT_SRC_NOT_AUTHORIZED);

            applyAllowTrust(app, gateway, a1, gatewaySeq++, "IDR", true);
            applyCreditPaymentTx(app, a1, gateway, idrCur, a1Seq++,
                                trustLineStartingBalance);
        }
    }

    SECTION("self allow trust")
    {
        SECTION("allow trust not required")
        {
            applyAllowTrust(app, gateway, gateway, gatewaySeq++, "IDR", true, ALLOW_TRUST_TRUST_NOT_REQUIRED);
            applyAllowTrust(app, gateway, gateway, gatewaySeq++, "IDR", false, ALLOW_TRUST_TRUST_NOT_REQUIRED);
        }

        SECTION("allow trust without explicit trustline")
        {
            auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
            applySetOptions(app, gateway, gatewaySeq++, nullptr, &setFlags,
                            nullptr, nullptr, nullptr, nullptr);

            SECTION("do not set revocable flag")
            {
                applyAllowTrust(app, gateway, gateway, gatewaySeq++, "IDR", true);
                applyAllowTrust(app, gateway, gateway, gatewaySeq++, "IDR", false, ALLOW_TRUST_CANT_REVOKE);
            }
            SECTION("set revocable flag")
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                applySetOptions(app, gateway, gatewaySeq++, nullptr, &setFlags,
                                nullptr, nullptr, nullptr, nullptr);

                applyAllowTrust(app, gateway, gateway, gatewaySeq++, "IDR", true);
                applyAllowTrust(app, gateway, gateway, gatewaySeq++, "IDR", false);
            }
        }
    }
}
