// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/XDROperators.h"

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
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto a1 = root.create("A", app->getLedgerManager().getMinBalance(0) + 1000);

    SECTION("Signers")
    {
        auto s1 = getAccount("S1");
        auto sk1 = makeSigner(s1, 1); // low right account
        auto th = setMasterWeight(100) | setLowThreshold(1) |
                  setMedThreshold(10) | setHighThreshold(100);

        SECTION("insufficient balance")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(th | setSigner(sk1)),
                                  ex_SET_OPTIONS_LOW_RESERVE);
            });
        }

        SECTION("add signer with native selling liabilities")
        {
            auto const minBal2 = app->getLedgerManager().getMinBalance(2);
            auto txfee = app->getLedgerManager().getTxFee();
            auto const native = makeNativeAsset();
            auto acc1 = root.create("acc1", minBal2 + 2 * txfee + 500 - 1);
            TestMarket market(*app);

            auto cur1 = acc1.asset("CUR1");
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc1, {native, cur1, Price{1, 1}, 500});
            });

            for_versions_to(9, *app,
                            [&] { acc1.setOptions(th | setSigner(sk1)); });
            for_versions_from(10, *app, [&] {
                REQUIRE_THROWS_AS(acc1.setOptions(th | setSigner(sk1)),
                                  ex_SET_OPTIONS_LOW_RESERVE);
                root.pay(acc1, txfee + 1);
                acc1.setOptions(th | setSigner(sk1));
            });
        }

        SECTION("add signer with native buying liabilities")
        {
            auto const minBal2 = app->getLedgerManager().getMinBalance(2);
            auto txfee = app->getLedgerManager().getTxFee();
            auto const native = makeNativeAsset();
            auto acc1 = root.create("acc1", minBal2 + 2 * txfee + 500 - 1);
            TestMarket market(*app);

            auto cur1 = acc1.asset("CUR1");
            market.requireChangesWithOffer({}, [&] {
                return market.addOffer(acc1, {cur1, native, Price{1, 1}, 500});
            });

            for_all_versions(*app,
                             [&] { acc1.setOptions(th | setSigner(sk1)); });
        }

        SECTION("can't use master key as alternate signer")
        {
            auto sk = makeSigner(a1, 100);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk)),
                                  ex_SET_OPTIONS_BAD_SIGNER);
            });
        }

        SECTION("bad weight for master key")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setMasterWeight(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            });
        }
        SECTION("bad thresholds")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setLowThreshold(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);

                REQUIRE_THROWS_AS(a1.setOptions(setMedThreshold(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);

                REQUIRE_THROWS_AS(a1.setOptions(setHighThreshold(256)),
                                  ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE);
            });
        }

        SECTION("invalid signer weight")
        {
            root.pay(a1, app->getLedgerManager().getMinBalance(2));

            auto sk1_over = makeSigner(s1, 256);
            for_versions_to(9, *app,
                            [&] { a1.setOptions(setSigner(sk1_over)); });
            for_versions_from(10, *app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk1_over)),
                                  ex_SET_OPTIONS_BAD_SIGNER);
            });
        }

        SECTION("non-account signers")
        {

            for_versions_to(2, *app, [&] {
                // add some funds
                root.pay(a1, app->getLedgerManager().getMinBalance(2));

                a1.setOptions(th | setSigner(sk1));

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
                auto s2 = getAccount("S2");
                auto sk2 = makeSigner(s2, 100);
                a1.setOptions(setSigner(sk2));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 2);
                REQUIRE(a1Account->getAccount().signers.size() == 2);

                // add signer 3 - non account, will fail for old ledger
                SignerKey s3;
                s3.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                Signer sk3(s3, 100);
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk3)),
                                  ex_SET_OPTIONS_BAD_SIGNER);

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 2);
                REQUIRE(a1Account->getAccount().signers.size() == 2);

                // update signer 2
                sk2.weight = 11;
                a1.setOptions(setSigner(sk2));

                // update signer 1
                sk1.weight = 11;
                a1.setOptions(setSigner(sk1));

                // remove signer 1
                sk1.weight = 0;
                a1.setOptions(setSigner(sk1));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 1);
                REQUIRE(a1Account->getAccount().signers.size() == 1);
                Signer& a_sk2 = a1Account->getAccount().signers[0];
                REQUIRE(a_sk2.key == sk2.key);
                REQUIRE(a_sk2.weight == sk2.weight);

                // remove signer 3 - non account, not added, because of old
                // ledger
                sk3.weight = 0;
                REQUIRE_THROWS_AS(a1.setOptions(setSigner(sk3)),
                                  ex_SET_OPTIONS_BAD_SIGNER);

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 1);
                REQUIRE(a1Account->getAccount().signers.size() == 1);

                // remove signer 2
                sk2.weight = 0;
                a1.setOptions(setSigner(sk2));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 0);
                REQUIRE(a1Account->getAccount().signers.size() == 0);
            });

            for_versions_from(3, *app, [&] {
                // add some funds
                root.pay(a1, app->getLedgerManager().getMinBalance(2));
                a1.setOptions(th | setSigner(sk1));

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
                auto s2 = getAccount("S2");
                auto sk2 = makeSigner(s2, 100);
                a1.setOptions(setSigner(sk2));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 2);
                REQUIRE(a1Account->getAccount().signers.size() == 2);

                // add signer 3 - non account
                SignerKey s3;
                s3.type(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                Signer sk3(s3, 100);
                a1.setOptions(setSigner(sk3));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 3);
                REQUIRE(a1Account->getAccount().signers.size() == 3);

                // update signer 2
                sk2.weight = 11;
                a1.setOptions(setSigner(sk2));

                // update signer 1
                sk1.weight = 11;
                a1.setOptions(setSigner(sk1));

                // remove signer 1
                sk1.weight = 0;
                a1.setOptions(setSigner(sk1));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 2);
                REQUIRE(a1Account->getAccount().signers.size() == 2);
                Signer& a_sk2 = a1Account->getAccount().signers[0];
                REQUIRE(a_sk2.key == sk2.key);
                REQUIRE(a_sk2.weight == sk2.weight);

                // remove signer 3 - non account
                sk3.weight = 0;
                a1.setOptions(setSigner(sk3));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 1);
                REQUIRE(a1Account->getAccount().signers.size() == 1);

                // remove signer 2
                sk2.weight = 0;
                a1.setOptions(setSigner(sk2));

                a1Account = loadAccount(a1, *app);
                REQUIRE(a1Account->getAccount().numSubEntries == 0);
                REQUIRE(a1Account->getAccount().signers.size() == 0);
            });
        }
    }

    SECTION("flags")
    {
        SECTION("Can't set and clear same flag")
        {
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(a1.setOptions(setFlags(AUTH_REQUIRED_FLAG) |
                                                clearFlags(AUTH_REQUIRED_FLAG)),
                                  ex_SET_OPTIONS_BAD_FLAGS);
            });
        }
        SECTION("auth flags")
        {
            for_all_versions(*app, [&] {
                a1.setOptions(setFlags(AUTH_REQUIRED_FLAG));
                a1.setOptions(setFlags(AUTH_REVOCABLE_FLAG));
                a1.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));
                a1.setOptions(setFlags(AUTH_IMMUTABLE_FLAG));

                // at this point trying to change any flag should fail
                REQUIRE_THROWS_AS(
                    a1.setOptions(clearFlags(AUTH_IMMUTABLE_FLAG)),
                    ex_SET_OPTIONS_CANT_CHANGE);
                REQUIRE_THROWS_AS(a1.setOptions(clearFlags(AUTH_REQUIRED_FLAG)),
                                  ex_SET_OPTIONS_CANT_CHANGE);
                REQUIRE_THROWS_AS(a1.setOptions(setFlags(AUTH_REVOCABLE_FLAG)),
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
                    REQUIRE_THROWS_AS(a1.setOptions(setHomeDomain(s)),
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
