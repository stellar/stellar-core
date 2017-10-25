// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

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
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto a1 = root.create("A", app->getLedgerManager().getMinBalance(0) + 1000);

    SECTION("Signers")
    {
        SecretKey s1 = getAccount("S1");
        Signer sk1(KeyUtils::convertKey<SignerKey>(s1.getPublicKey()),
                   1); // low right account

        ThresholdSetter th;

        th.masterWeight = make_optional<int>(100);
        th.lowThreshold = make_optional<int>(1);
        th.medThreshold = make_optional<int>(10);
        th.highThreshold = make_optional<int>(100);

        SECTION("insufficient balance")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, nullptr, &th,
                                                &sk1, nullptr),
                                  ex_SET_OPTIONS_LOW_RESERVE);
            });
        }

        SECTION("can't use master key as alternate signer")
        {
            Signer sk(KeyUtils::convertKey<SignerKey>(a1.getPublicKey()), 100);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, nullptr,
                                                nullptr, &sk, nullptr),
                                  ex_SET_OPTIONS_BAD_SIGNER);
            });
        }

        for_versions_to(2, *app, [&] {
            // add some funds
            root.pay(a1, app->getLedgerManager().getMinBalance(2));

            a1.setOptions(nullptr, nullptr, nullptr, &th, &sk1, nullptr);

            AccountFrame::pointer a1Account;

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 1);
            REQUIRE(a1Account->getAccount().signers.size() == 1);
            {
                Signer& a_sk1 = a1Account->getAccount().signers[0];
                REQUIRE(a_sk1.key == sk1.key);
                REQUIRE(a_sk1.weight == sk1.weight);
            }

            // add signer 2
            SecretKey s2 = getAccount("S2");
            Signer sk2(KeyUtils::convertKey<SignerKey>(s2.getPublicKey()), 100);
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 2);
            REQUIRE(a1Account->getAccount().signers.size() == 2);

            // add signer 3 - non account, will fail for old ledger
            SignerKey s3;
            s3.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
            Signer sk3(s3, 100);
            REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                            &sk3, nullptr),
                              ex_SET_OPTIONS_BAD_SIGNER);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 2);
            REQUIRE(a1Account->getAccount().signers.size() == 2);

            // update signer 2
            sk2.weight = 11;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

            // update signer 1
            sk1.weight = 11;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk1, nullptr);

            // remove signer 1
            sk1.weight = 0;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk1, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 1);
            REQUIRE(a1Account->getAccount().signers.size() == 1);
            Signer& a_sk2 = a1Account->getAccount().signers[0];
            REQUIRE(a_sk2.key == sk2.key);
            REQUIRE(a_sk2.weight == sk2.weight);

            // remove signer 3 - non account, not added, because of old ledger
            sk3.weight = 0;
            REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, nullptr, nullptr,
                                            &sk3, nullptr),
                              ex_SET_OPTIONS_BAD_SIGNER);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 1);
            REQUIRE(a1Account->getAccount().signers.size() == 1);

            // remove signer 2
            sk2.weight = 0;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 0);
            REQUIRE(a1Account->getAccount().signers.size() == 0);
        });

        for_versions_from(3, *app, [&] {
            // add some funds
            root.pay(a1, app->getLedgerManager().getMinBalance(2));
            a1.setOptions(nullptr, nullptr, nullptr, &th, &sk1, nullptr);

            AccountFrame::pointer a1Account;

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 1);
            REQUIRE(a1Account->getAccount().signers.size() == 1);
            {
                Signer& a_sk1 = a1Account->getAccount().signers[0];
                REQUIRE(a_sk1.key == sk1.key);
                REQUIRE(a_sk1.weight == sk1.weight);
            }

            // add signer 2
            SecretKey s2 = getAccount("S2");
            Signer sk2(KeyUtils::convertKey<SignerKey>(s2.getPublicKey()), 100);
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 2);
            REQUIRE(a1Account->getAccount().signers.size() == 2);

            // add signer 3 - non account
            SignerKey s3;
            s3.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
            Signer sk3(s3, 100);
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk3, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 3);
            REQUIRE(a1Account->getAccount().signers.size() == 3);

            // update signer 2
            sk2.weight = 11;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

            // update signer 1
            sk1.weight = 11;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk1, nullptr);

            // remove signer 1
            sk1.weight = 0;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk1, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 2);
            REQUIRE(a1Account->getAccount().signers.size() == 2);
            Signer& a_sk2 = a1Account->getAccount().signers[0];
            REQUIRE(a_sk2.key == sk2.key);
            REQUIRE(a_sk2.weight == sk2.weight);

            // remove signer 3 - non account
            sk3.weight = 0;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk3, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 1);
            REQUIRE(a1Account->getAccount().signers.size() == 1);

            // remove signer 2
            sk2.weight = 0;
            a1.setOptions(nullptr, nullptr, nullptr, nullptr, &sk2, nullptr);

            a1Account = loadAccount(a1, *app);
            REQUIRE(a1Account->getAccount().numSubEntries == 0);
            REQUIRE(a1Account->getAccount().signers.size() == 0);
        });
    }

    SECTION("flags")
    {
        SECTION("Can't set and clear same flag")
        {
            for_all_versions(*app, [&] {
                uint32_t setFlags = AUTH_REQUIRED_FLAG;
                uint32_t clearFlags = AUTH_REQUIRED_FLAG;
                REQUIRE_THROWS_AS(a1.setOptions(nullptr, &setFlags, &clearFlags,
                                                nullptr, nullptr, nullptr),
                                  ex_SET_OPTIONS_BAD_FLAGS);
            });
        }
        SECTION("auth flags")
        {
            for_all_versions(*app, [&] {
                uint32_t flags;

                flags = AUTH_REQUIRED_FLAG;
                a1.setOptions(nullptr, &flags, nullptr, nullptr, nullptr,
                              nullptr);

                flags = AUTH_REVOCABLE_FLAG;
                a1.setOptions(nullptr, &flags, nullptr, nullptr, nullptr,
                              nullptr);

                // clear flag
                a1.setOptions(nullptr, nullptr, &flags, nullptr, nullptr,
                              nullptr);

                flags = AUTH_IMMUTABLE_FLAG;
                a1.setOptions(nullptr, &flags, nullptr, nullptr, nullptr,
                              nullptr);

                // at this point trying to change any flag should fail

                REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, &flags,
                                                nullptr, nullptr, nullptr),
                                  ex_SET_OPTIONS_CANT_CHANGE);

                flags = AUTH_REQUIRED_FLAG;
                REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, &flags,
                                                nullptr, nullptr, nullptr),
                                  ex_SET_OPTIONS_CANT_CHANGE);

                flags = AUTH_REVOCABLE_FLAG;
                REQUIRE_THROWS_AS(a1.setOptions(nullptr, &flags, nullptr,
                                                nullptr, nullptr, nullptr),
                                  ex_SET_OPTIONS_CANT_CHANGE);
            });
        }
    }

    SECTION("Home domain")
    {
        SECTION("invalid home domain")
        {
            for_all_versions(*app, [&] {
                std::string bad[] = {"abc\r", "abc\x7F",
                                     std::string("ab\000c", 4)};
                for (auto& s : bad)
                {
                    REQUIRE_THROWS_AS(a1.setOptions(nullptr, nullptr, nullptr,
                                                    nullptr, nullptr, &s),
                                      ex_SET_OPTIONS_INVALID_HOME_DOMAIN);
                }
            });
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
