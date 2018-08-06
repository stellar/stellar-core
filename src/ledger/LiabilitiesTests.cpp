// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AccountFrame.h"
#include "ledger/LedgerTestUtils.h"
#include "ledger/TrustFrame.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Timer.h"

using namespace stellar;

TEST_CASE("liabilities", "[ledger][liabilities]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& lm = app->getLedgerManager();
    app->start();

    SECTION("add account selling liabilities")
    {
        auto addSellingLiabilities =
            [&](uint32_t initNumSubEntries, int64_t initBalance,
                int64_t initSellingLiabilities, int64_t deltaLiabilities) {
                AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
                ae.balance = initBalance;
                ae.numSubEntries = initNumSubEntries;
                if (ae.ext.v() < 1)
                {
                    ae.ext.v(1);
                }
                ae.ext.v1().liabilities.selling = initSellingLiabilities;
                int64_t initBuyingLiabilities = ae.ext.v1().liabilities.buying;

                LedgerEntry le;
                le.data.type(ACCOUNT);
                le.data.account() = ae;

                auto af = std::make_shared<AccountFrame>(le);
                bool res = af->addSellingLiabilities(deltaLiabilities, lm);
                REQUIRE(af->getBalance() == initBalance);
                REQUIRE(af->getBuyingLiabilities(lm) == initBuyingLiabilities);
                if (res)
                {
                    REQUIRE(af->getSellingLiabilities(lm) ==
                            initSellingLiabilities + deltaLiabilities);
                }
                else
                {
                    REQUIRE(af->getSellingLiabilities(lm) ==
                            initSellingLiabilities);
                    REQUIRE(af->getAccount() == ae);
                }
                return res;
            };
        auto addSellingLiabilitiesUninitialized =
            [&](uint32_t initNumSubEntries, int64_t initBalance,
                int64_t deltaLiabilities) {
                AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
                ae.balance = initBalance;
                ae.numSubEntries = initNumSubEntries;
                ae.ext.v(0);

                LedgerEntry le;
                le.data.type(ACCOUNT);
                le.data.account() = ae;

                auto af = std::make_shared<AccountFrame>(le);
                bool res = af->addSellingLiabilities(deltaLiabilities, lm);
                REQUIRE(af->getBalance() == initBalance);
                REQUIRE(af->getBuyingLiabilities(lm) == 0);
                if (res)
                {
                    REQUIRE(af->getAccount().ext.v() ==
                            ((deltaLiabilities != 0) ? 1 : 0));
                    REQUIRE(af->getSellingLiabilities(lm) == deltaLiabilities);
                }
                else
                {
                    REQUIRE(af->getSellingLiabilities(lm) == 0);
                    REQUIRE(af->getAccount() == ae);
                }
                return res;
            };

        for_versions_from(10, *app, [&] {
            SECTION("uninitialized liabilities")
            {
                // Uninitialized remains uninitialized after failure
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 1));

                // Uninitialized remains unitialized after success of delta 0
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 0));

                // Uninitialized is initialized after success of delta != 0
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1, 1));
            }

            SECTION("below reserve")
            {
                // Can leave unchanged when account is below reserve
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) - 1, 0, 0));

                // Cannot increase when account is below reserve
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) - 1, 0, 1));

                // No need to test decrease below reserve since that would imply
                // the previous state had excess liabilities
            }

            SECTION("cannot make liabilities negative")
            {
                // No initial liabilities and at maximum
                REQUIRE(addSellingLiabilities(0, lm.getMinBalance(0), 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 0));
                REQUIRE(!addSellingLiabilities(0, lm.getMinBalance(0), 0, -1));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), -1));

                // No initial liabilities and below maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) + 1, 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1, 0));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) + 1, 0, -1));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1, -1));

                // Initial liabilities and at maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) + 1, 1, -1));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) + 1, 1, -2));

                // Initial liabilities and below maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) + 2, 1, -1));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) + 2, 1, -2));
            }

            SECTION("cannot increase liabilities above balance minus reserve")
            {
                // No initial liabilities and at maximum
                REQUIRE(addSellingLiabilities(0, lm.getMinBalance(0), 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 0));
                REQUIRE(!addSellingLiabilities(0, lm.getMinBalance(0), 0, 1));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 1));

                // No initial liabilities and below maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) + 1, 0, 1));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1, 1));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) + 1, 0, 2));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1, 2));

                // Initial liabilities and at maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) + 1, 1, 0));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) + 1, 1, 1));

                // Initial liabilities and below maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getMinBalance(0) + 2, 1, 1));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getMinBalance(0) + 2, 1, 2));
            }

            SECTION("limiting values")
            {
                // Can increase to limit but no higher
                REQUIRE(addSellingLiabilities(0, INT64_MAX, 0,
                                              INT64_MAX - lm.getMinBalance(0)));
                REQUIRE(!addSellingLiabilities(
                    0, INT64_MAX, 0, INT64_MAX - lm.getMinBalance(0) + 1));
                REQUIRE(!addSellingLiabilities(
                    0, INT64_MAX, 1, INT64_MAX - lm.getMinBalance(0)));

                // Can decrease from limit
                REQUIRE(addSellingLiabilities(
                    0, INT64_MAX, INT64_MAX - lm.getMinBalance(0), -1));
                REQUIRE(addSellingLiabilities(0, INT64_MAX,
                                              INT64_MAX - lm.getMinBalance(0),
                                              lm.getMinBalance(0) - INT64_MAX));
            }
        });
    }

    SECTION("add account buying liabilities")
    {
        auto addBuyingLiabilities = [&](uint32_t initNumSubEntries,
                                        int64_t initBalance,
                                        int64_t initBuyingLiabilities,
                                        int64_t deltaLiabilities) {
            AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
            ae.balance = initBalance;
            ae.numSubEntries = initNumSubEntries;
            if (ae.ext.v() < 1)
            {
                ae.ext.v(1);
            }
            ae.ext.v1().liabilities.buying = initBuyingLiabilities;
            int64_t initSellingLiabilities = ae.ext.v1().liabilities.selling;

            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account() = ae;

            auto af = std::make_shared<AccountFrame>(le);
            bool res = af->addBuyingLiabilities(deltaLiabilities, lm);
            REQUIRE(af->getBalance() == initBalance);
            REQUIRE(af->getSellingLiabilities(lm) == initSellingLiabilities);
            if (res)
            {
                REQUIRE(af->getBuyingLiabilities(lm) ==
                        initBuyingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(af->getBuyingLiabilities(lm) == initBuyingLiabilities);
                REQUIRE(af->getAccount() == ae);
            }
            return res;
        };
        auto addBuyingLiabilitiesUninitialized = [&](uint32_t initNumSubEntries,
                                                     int64_t initBalance,
                                                     int64_t deltaLiabilities) {
            AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
            ae.balance = initBalance;
            ae.numSubEntries = initNumSubEntries;
            ae.ext.v(0);

            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account() = ae;

            auto af = std::make_shared<AccountFrame>(le);
            bool res = af->addBuyingLiabilities(deltaLiabilities, lm);
            REQUIRE(af->getBalance() == initBalance);
            REQUIRE(af->getSellingLiabilities(lm) == 0);
            if (res)
            {
                REQUIRE(af->getAccount().ext.v() ==
                        ((deltaLiabilities != 0) ? 1 : 0));
                REQUIRE(af->getBuyingLiabilities(lm) == deltaLiabilities);
            }
            else
            {
                REQUIRE(af->getBuyingLiabilities(lm) == 0);
                REQUIRE(af->getAccount() == ae);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("uninitialized liabilities")
            {
                // Uninitialized remains uninitialized after failure
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0),
                    INT64_MAX - (lm.getMinBalance(0) - 1)));

                // Uninitialized remains uninitialized after success of delta 0
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 0));

                // Uninitialized is initialized after success of delta != 0
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 1));
            }

            SECTION("below reserve")
            {
                // Can decrease when account is below reserve
                REQUIRE(
                    addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 1, -1));

                // Can leave unchanged when account is below reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 0, 0));
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 1, 0));

                // Can increase when account is below reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 0, 1));
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 1, 1));
            }

            SECTION("cannot make liabilities negative")
            {
                // No initial liabilities
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0), 0, 0));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), 0));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0), 0, -1));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), -1));

                // Initial liabilities
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0), 1, -1));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0), 1, -2));
            }

            SECTION("cannot increase liabilities above INT64_MAX minus balance")
            {
                // No initial liabilities, account below reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 0,
                                             INT64_MAX -
                                                 (lm.getMinBalance(0) - 1)));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) - 1,
                    INT64_MAX - (lm.getMinBalance(0) - 1)));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 0,
                                              INT64_MAX -
                                                  (lm.getMinBalance(0) - 2)));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) - 1,
                    INT64_MAX - (lm.getMinBalance(0) - 2)));

                // Initial liabilities, account below reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 1,
                                             INT64_MAX - lm.getMinBalance(0)));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0) - 1, 1,
                                              INT64_MAX -
                                                  (lm.getMinBalance(0) - 1)));

                // No initial liabilities, account at reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0), 0,
                                             INT64_MAX - lm.getMinBalance(0)));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0), INT64_MAX - lm.getMinBalance(0)));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0), 0,
                                              INT64_MAX -
                                                  (lm.getMinBalance(0) - 1)));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0),
                    INT64_MAX - (lm.getMinBalance(0) - 1)));

                // Initial liabilities, account at reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0), 1,
                                             INT64_MAX -
                                                 (lm.getMinBalance(0) + 1)));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0), 1,
                                              INT64_MAX - lm.getMinBalance(0)));

                // No initial liabilities, account above reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) + 1, 0,
                                             INT64_MAX -
                                                 (lm.getMinBalance(0) + 1)));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1,
                    INT64_MAX - (lm.getMinBalance(0) + 1)));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0) + 1, 0,
                                              INT64_MAX - lm.getMinBalance(0)));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getMinBalance(0) + 1,
                    INT64_MAX - lm.getMinBalance(0)));

                // Initial liabilities, account above reserve
                REQUIRE(addBuyingLiabilities(0, lm.getMinBalance(0) + 1, 1,
                                             INT64_MAX -
                                                 (lm.getMinBalance(0) + 2)));
                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0) + 1, 1,
                                              INT64_MAX -
                                                  (lm.getMinBalance(0) + 1)));
            }

            SECTION("limiting values")
            {
                REQUIRE(!addBuyingLiabilities(0, INT64_MAX, 0, 1));
                REQUIRE(addBuyingLiabilities(0, INT64_MAX - 1, 0, 1));

                REQUIRE(!addBuyingLiabilities(0, lm.getMinBalance(0),
                                              INT64_MAX - lm.getMinBalance(0),
                                              1));
                REQUIRE(addBuyingLiabilities(
                    0, lm.getMinBalance(0), INT64_MAX - lm.getMinBalance(0) - 1,
                    1));

                REQUIRE(!addBuyingLiabilities(UINT32_MAX, INT64_MAX / 2 + 1,
                                              INT64_MAX / 2, 1));
                REQUIRE(!addBuyingLiabilities(UINT32_MAX, INT64_MAX / 2,
                                              INT64_MAX / 2 + 1, 1));
                REQUIRE(addBuyingLiabilities(UINT32_MAX, INT64_MAX / 2,
                                             INT64_MAX / 2, 1));
            }
        });
    }

    SECTION("add trustline selling liabilities")
    {
        auto addSellingLiabilities = [&](int64_t initLimit, int64_t initBalance,
                                         int64_t initSellingLiabilities,
                                         int64_t deltaLiabilities) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.flags = AUTHORIZED_FLAG;
            tl.balance = initBalance;
            tl.limit = initLimit;
            if (tl.ext.v() < 1)
            {
                tl.ext.v(1);
            }
            tl.ext.v1().liabilities.selling = initSellingLiabilities;
            int64_t initBuyingLiabilities = tl.ext.v1().liabilities.buying;

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            bool res = tf->addSellingLiabilities(deltaLiabilities, lm);
            REQUIRE(tf->getTrustLine().limit == initLimit);
            REQUIRE(tf->getBalance() == initBalance);
            REQUIRE(tf->getBuyingLiabilities(lm) == initBuyingLiabilities);
            if (res)
            {
                REQUIRE(tf->getSellingLiabilities(lm) ==
                        initSellingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(tf->getSellingLiabilities(lm) ==
                        initSellingLiabilities);
                REQUIRE(tf->getTrustLine() == tl);
            }
            return res;
        };
        auto addSellingLiabilitiesUninitialized =
            [&](int64_t initLimit, int64_t initBalance,
                int64_t deltaLiabilities) {
                TrustLineEntry tl =
                    LedgerTestUtils::generateValidTrustLineEntry();
                tl.flags = AUTHORIZED_FLAG;
                tl.balance = initBalance;
                tl.limit = initLimit;
                tl.ext.v(0);

                LedgerEntry le;
                le.data.type(TRUSTLINE);
                le.data.trustLine() = tl;

                auto tf = std::make_shared<TrustFrame>(le);
                bool res = tf->addSellingLiabilities(deltaLiabilities, lm);
                REQUIRE(tf->getTrustLine().limit == initLimit);
                REQUIRE(tf->getBalance() == initBalance);
                REQUIRE(tf->getBuyingLiabilities(lm) == 0);
                if (res)
                {
                    REQUIRE(tf->getTrustLine().ext.v() ==
                            ((deltaLiabilities != 0) ? 1 : 0));
                    REQUIRE(tf->getSellingLiabilities(lm) == deltaLiabilities);
                }
                else
                {
                    REQUIRE(tf->getSellingLiabilities(lm) == 0);
                    REQUIRE(tf->getTrustLine() == tl);
                }
                return res;
            };

        for_versions_from(10, *app, [&] {
            SECTION("uninitialized liabilities")
            {
                // Uninitialized remains uninitialized after failure
                REQUIRE(!addSellingLiabilitiesUninitialized(1, 0, 1));

                // Uninitialized remains unitialized after success of delta 0
                REQUIRE(addSellingLiabilitiesUninitialized(1, 1, 0));

                // Uninitialized is initialized after success of delta != 0
                REQUIRE(addSellingLiabilitiesUninitialized(1, 1, 1));
            }

            SECTION("cannot make liabilities negative")
            {
                // No initial liabilities
                REQUIRE(addSellingLiabilities(1, 0, 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(1, 0, 0));
                REQUIRE(!addSellingLiabilities(1, 0, 0, -1));
                REQUIRE(!addSellingLiabilitiesUninitialized(1, 1, -1));

                // Initial liabilities
                REQUIRE(addSellingLiabilities(1, 0, 1, -1));
                REQUIRE(!addSellingLiabilities(1, 0, 1, -2));
            }

            SECTION("cannot increase liabilities above balance")
            {
                // No initial liabilities, below maximum
                REQUIRE(addSellingLiabilities(2, 1, 0, 1));
                REQUIRE(addSellingLiabilitiesUninitialized(2, 1, 1));
                REQUIRE(!addSellingLiabilities(2, 1, 0, 2));
                REQUIRE(!addSellingLiabilitiesUninitialized(2, 1, 2));

                // Initial liabilities, below maximum
                REQUIRE(addSellingLiabilities(2, 2, 1, 1));
                REQUIRE(!addSellingLiabilities(2, 2, 1, 2));

                // No initial liabilities, at maximum
                REQUIRE(addSellingLiabilities(2, 0, 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(2, 0, 0));
                REQUIRE(!addSellingLiabilities(2, 0, 0, 1));
                REQUIRE(!addSellingLiabilitiesUninitialized(2, 0, 1));

                // Initial liabilities, at maximum
                REQUIRE(addSellingLiabilities(2, 2, 2, 0));
                REQUIRE(!addSellingLiabilities(2, 2, 2, 1));
            }

            SECTION("limiting values")
            {
                REQUIRE(
                    addSellingLiabilities(INT64_MAX, INT64_MAX, 0, INT64_MAX));
                REQUIRE(!addSellingLiabilities(INT64_MAX, INT64_MAX - 1, 0,
                                               INT64_MAX));
                REQUIRE(addSellingLiabilities(INT64_MAX, INT64_MAX - 1, 0,
                                              INT64_MAX - 1));

                REQUIRE(
                    !addSellingLiabilities(INT64_MAX, INT64_MAX, INT64_MAX, 1));
                REQUIRE(addSellingLiabilities(INT64_MAX, INT64_MAX,
                                              INT64_MAX - 1, 1));
            }
        });
    }

    SECTION("add trustline buying liabilities")
    {
        auto addBuyingLiabilities = [&](int64_t initLimit, int64_t initBalance,
                                        int64_t initBuyingLiabilities,
                                        int64_t deltaLiabilities) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.flags = AUTHORIZED_FLAG;
            tl.balance = initBalance;
            tl.limit = initLimit;
            if (tl.ext.v() < 1)
            {
                tl.ext.v(1);
            }
            tl.ext.v1().liabilities.buying = initBuyingLiabilities;
            int64_t initSellingLiabilities = tl.ext.v1().liabilities.selling;

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            bool res = tf->addBuyingLiabilities(deltaLiabilities, lm);
            REQUIRE(tf->getTrustLine().limit == initLimit);
            REQUIRE(tf->getBalance() == initBalance);
            REQUIRE(tf->getSellingLiabilities(lm) == initSellingLiabilities);
            if (res)
            {
                REQUIRE(tf->getBuyingLiabilities(lm) ==
                        initBuyingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(tf->getBuyingLiabilities(lm) == initBuyingLiabilities);
                REQUIRE(tf->getTrustLine() == tl);
            }
            return res;
        };
        auto addBuyingLiabilitiesUninitialized = [&](int64_t initLimit,
                                                     int64_t initBalance,
                                                     int64_t deltaLiabilities) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.flags = AUTHORIZED_FLAG;
            tl.balance = initBalance;
            tl.limit = initLimit;
            tl.ext.v(0);

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            bool res = tf->addBuyingLiabilities(deltaLiabilities, lm);
            REQUIRE(tf->getTrustLine().limit == initLimit);
            REQUIRE(tf->getBalance() == initBalance);
            REQUIRE(tf->getSellingLiabilities(lm) == 0);
            if (res)
            {
                REQUIRE(tf->getTrustLine().ext.v() ==
                        ((deltaLiabilities != 0) ? 1 : 0));
                REQUIRE(tf->getBuyingLiabilities(lm) == deltaLiabilities);
            }
            else
            {
                REQUIRE(tf->getBuyingLiabilities(lm) == 0);
                REQUIRE(tf->getTrustLine() == tl);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("uninitialized liabilities")
            {
                // Uninitialized remains uninitialized after failure
                REQUIRE(!addBuyingLiabilitiesUninitialized(1, 0, 2));

                // Uninitialized remains unitialized after success of delta 0
                REQUIRE(addBuyingLiabilitiesUninitialized(1, 0, 0));

                // Uninitialized is initialized after success of delta != 0
                REQUIRE(addBuyingLiabilitiesUninitialized(1, 0, 1));
            }

            SECTION("cannot make liabilities negative")
            {
                // No initial liabilities
                REQUIRE(addBuyingLiabilities(1, 0, 0, 0));
                REQUIRE(addBuyingLiabilitiesUninitialized(1, 0, 0));
                REQUIRE(!addBuyingLiabilities(1, 0, 0, -1));
                REQUIRE(!addBuyingLiabilitiesUninitialized(1, 1, -1));

                // Initial liabilities
                REQUIRE(addBuyingLiabilities(1, 0, 1, -1));
                REQUIRE(!addBuyingLiabilities(1, 0, 1, -2));
            }

            SECTION("cannot increase liabilities above limit minus balance")
            {
                // No initial liabilities, below maximum
                REQUIRE(addBuyingLiabilities(2, 1, 0, 1));
                REQUIRE(addBuyingLiabilitiesUninitialized(2, 1, 1));
                REQUIRE(!addBuyingLiabilities(2, 1, 0, 2));
                REQUIRE(!addBuyingLiabilitiesUninitialized(2, 1, 2));

                // Initial liabilities, below maximum
                REQUIRE(addBuyingLiabilities(3, 1, 1, 1));
                REQUIRE(!addBuyingLiabilities(3, 1, 1, 2));

                // No initial liabilities, at maximum
                REQUIRE(addBuyingLiabilities(2, 2, 0, 0));
                REQUIRE(addBuyingLiabilitiesUninitialized(2, 2, 0));
                REQUIRE(!addBuyingLiabilities(2, 2, 0, 1));
                REQUIRE(!addBuyingLiabilitiesUninitialized(2, 2, 1));

                // Initial liabilities, at maximum
                REQUIRE(addBuyingLiabilities(3, 2, 1, 0));
                REQUIRE(!addBuyingLiabilities(3, 2, 1, 1));
            }

            SECTION("limiting values")
            {
                REQUIRE(!addBuyingLiabilities(INT64_MAX, INT64_MAX, 0, 1));
                REQUIRE(addBuyingLiabilities(INT64_MAX, INT64_MAX - 1, 0, 1));

                REQUIRE(!addBuyingLiabilities(INT64_MAX, 0, INT64_MAX, 1));
                REQUIRE(addBuyingLiabilities(INT64_MAX, 0, INT64_MAX - 1, 1));

                REQUIRE(!addBuyingLiabilities(INT64_MAX, INT64_MAX / 2 + 1,
                                              INT64_MAX / 2, 1));
                REQUIRE(!addBuyingLiabilities(INT64_MAX, INT64_MAX / 2,
                                              INT64_MAX / 2 + 1, 1));
                REQUIRE(addBuyingLiabilities(INT64_MAX, INT64_MAX / 2,
                                             INT64_MAX / 2, 1));
            }
        });
    }
}

TEST_CASE("balance with liabilities", "[ledger][liabilities]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& lm = app->getLedgerManager();
    app->start();

    SECTION("account add balance")
    {
        auto addBalance = [&](uint32_t initNumSubEntries, int64_t initBalance,
                              Liabilities initLiabilities,
                              int64_t deltaBalance) {
            AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
            ae.balance = initBalance;
            ae.numSubEntries = initNumSubEntries;
            ae.ext.v(1);
            ae.ext.v1().liabilities = initLiabilities;

            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account() = ae;

            auto af = std::make_shared<AccountFrame>(le);
            bool res = af->addBalance(deltaBalance, lm);
            REQUIRE(af->getSellingLiabilities(lm) == initLiabilities.selling);
            REQUIRE(af->getBuyingLiabilities(lm) == initLiabilities.buying);
            if (res)
            {
                REQUIRE(af->getBalance() == initBalance + deltaBalance);
            }
            else
            {
                REQUIRE(af->getBalance() == initBalance);
                REQUIRE(af->getAccount() == ae);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("can increase balance from below minimum")
            {
                // Balance can remain unchanged below reserve
                REQUIRE(addBalance(0, lm.getMinBalance(0) - 1,
                                   Liabilities{0, 0}, 0));

                // Balance starts and ends below reserve
                REQUIRE(addBalance(0, lm.getMinBalance(0) - 2,
                                   Liabilities{0, 0}, 1));

                // Balance starts below reserve and ends at reserve
                REQUIRE(addBalance(0, lm.getMinBalance(0) - 1,
                                   Liabilities{0, 0}, 1));

                // Balance starts below reserve and ends above reserve
                REQUIRE(addBalance(0, lm.getMinBalance(0) - 1,
                                   Liabilities{0, 0}, 2));
            }

            SECTION("cannot decrease balance below reserve plus selling "
                    "liabilities")
            {
                // Below minimum balance, no liabilities
                REQUIRE(addBalance(0, lm.getMinBalance(0) - 1,
                                   Liabilities{0, 0}, 0));
                REQUIRE(!addBalance(0, lm.getMinBalance(0) - 1,
                                    Liabilities{0, 0}, -1));

                // Minimum balance, no liabilities
                REQUIRE(
                    addBalance(0, lm.getMinBalance(0), Liabilities{0, 0}, 0));
                REQUIRE(
                    !addBalance(0, lm.getMinBalance(0), Liabilities{0, 0}, -1));

                // Above minimum balance, no liabilities
                REQUIRE(addBalance(0, lm.getMinBalance(0) + 1,
                                   Liabilities{0, 0}, -1));
                REQUIRE(!addBalance(0, lm.getMinBalance(0) + 1,
                                    Liabilities{0, 0}, -2));

                // Above minimum balance, with liabilities
                REQUIRE(addBalance(0, lm.getMinBalance(0) + 1,
                                   Liabilities{0, 1}, 0));
                REQUIRE(!addBalance(0, lm.getMinBalance(0) + 1,
                                    Liabilities{0, 1}, -1));
            }

            SECTION("cannot increase balance above INT64_MAX minus buying "
                    "liabilities")
            {
                // Maximum balance, no liabilities
                REQUIRE(addBalance(0, INT64_MAX, Liabilities{0, 0}, 0));
                REQUIRE(!addBalance(0, INT64_MAX, Liabilities{0, 0}, 1));

                // Below maximum balance, no liabilities
                REQUIRE(addBalance(0, INT64_MAX - 1, Liabilities{0, 0}, 1));
                REQUIRE(!addBalance(0, INT64_MAX - 1, Liabilities{0, 0}, 2));

                // Below maximum balance, with liabilities
                REQUIRE(addBalance(0, INT64_MAX - 1, Liabilities{1, 0}, 0));
                REQUIRE(!addBalance(0, INT64_MAX - 1, Liabilities{1, 0}, 1));
            }
        });
    }

    SECTION("account add subentries")
    {
        auto addSubEntries = [&](uint32_t initNumSubEntries,
                                 int64_t initBalance,
                                 int64_t initSellingLiabilities,
                                 int32_t deltaNumSubEntries) {
            AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
            ae.balance = initBalance;
            ae.numSubEntries = initNumSubEntries;
            if (ae.ext.v() == 0)
            {
                ae.ext.v(1);
            }
            ae.ext.v1().liabilities.selling = initSellingLiabilities;
            int64_t initBuyingLiabilities = ae.ext.v1().liabilities.buying;

            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account() = ae;

            auto af = std::make_shared<AccountFrame>(le);
            bool res = af->addNumEntries(deltaNumSubEntries, lm);
            REQUIRE(af->getSellingLiabilities(lm) == initSellingLiabilities);
            REQUIRE(af->getBuyingLiabilities(lm) == initBuyingLiabilities);
            REQUIRE(af->getBalance() == initBalance);
            if (res)
            {
                if (deltaNumSubEntries > 0)
                {
                    REQUIRE(af->getAvailableBalance(lm) >= 0);
                }
            }
            else
            {
                REQUIRE(af->getAccount() == ae);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("can decrease sub entries when below min balance")
            {
                // Below reserve and below new reserve
                REQUIRE(addSubEntries(1, 0, 0, -1));
                REQUIRE(addSubEntries(1, lm.getMinBalance(0) - 1, 0, -1));

                // Below reserve but at new reserve
                REQUIRE(addSubEntries(1, lm.getMinBalance(0), 0, -1));

                // Below reserve but above new reserve
                REQUIRE(addSubEntries(1, lm.getMinBalance(1) - 1, 0, -1));
                REQUIRE(addSubEntries(1, lm.getMinBalance(0) + 1, 0, -1));
            }

            SECTION("cannot add sub entry without sufficient balance")
            {
                // Below reserve, no liabilities
                REQUIRE(!addSubEntries(0, lm.getMinBalance(0) - 1, 0, 1));

                // At reserve, no liabilities
                REQUIRE(!addSubEntries(0, lm.getMinBalance(0), 0, 1));

                // Above reserve but below new reserve, no liabilities
                REQUIRE(!addSubEntries(0, lm.getMinBalance(0) + 1, 0, 1));
                REQUIRE(!addSubEntries(0, lm.getMinBalance(1) - 1, 0, 1));

                // Above reserve but below new reserve, with liabilities
                REQUIRE(!addSubEntries(0, lm.getMinBalance(0) + 2, 1, 1));
                REQUIRE(!addSubEntries(0, lm.getMinBalance(1), 1, 1));

                // Above reserve but at new reserve, no liabilities
                REQUIRE(addSubEntries(0, lm.getMinBalance(1), 0, 1));

                // Above reserve but at new reserve, with liabilities
                REQUIRE(addSubEntries(0, lm.getMinBalance(1) + 1, 1, 1));

                // Above reserve and above new reserve, no liabilities
                REQUIRE(addSubEntries(0, lm.getMinBalance(1) + 1, 0, 1));

                // Above reserve and above new reserve, with liabilities
                REQUIRE(addSubEntries(0, lm.getMinBalance(1) + 1, 1, 1));
                REQUIRE(!addSubEntries(0, lm.getMinBalance(1) + 1, 2, 1));
            }
        });
    }

    SECTION("trustline add balance")
    {
        auto addBalance = [&](int64_t initLimit, int64_t initBalance,
                              Liabilities initLiabilities,
                              int64_t deltaBalance) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.balance = initBalance;
            tl.limit = initLimit;
            tl.flags = AUTHORIZED_FLAG;
            tl.ext.v(1);
            tl.ext.v1().liabilities = initLiabilities;

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            bool res = tf->addBalance(deltaBalance, lm);
            REQUIRE(tf->getSellingLiabilities(lm) == initLiabilities.selling);
            REQUIRE(tf->getBuyingLiabilities(lm) == initLiabilities.buying);
            if (res)
            {
                REQUIRE(tf->getBalance() == initBalance + deltaBalance);
            }
            else
            {
                REQUIRE(tf->getBalance() == initBalance);
                REQUIRE(tf->getTrustLine() == tl);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("cannot decrease balance below selling liabilities")
            {
                // No balance, no liabilities
                REQUIRE(addBalance(2, 0, Liabilities{0, 0}, 0));
                REQUIRE(!addBalance(2, 0, Liabilities{0, 0}, -1));

                // Balance, no liabilities
                REQUIRE(addBalance(2, 1, Liabilities{0, 0}, -1));
                REQUIRE(!addBalance(2, 1, Liabilities{0, 0}, -2));

                // Balance, liabilities
                REQUIRE(addBalance(2, 2, Liabilities{0, 1}, -1));
                REQUIRE(!addBalance(2, 2, Liabilities{0, 1}, -2));
            }

            SECTION(
                "cannot increase balance above limit minus buying liabilities")
            {
                // Maximum balance, no liabilities
                REQUIRE(addBalance(2, 2, Liabilities{0, 0}, 0));
                REQUIRE(!addBalance(2, 2, Liabilities{0, 0}, 1));

                // Below maximum balance, no liabilities
                REQUIRE(addBalance(2, 1, Liabilities{0, 0}, 1));
                REQUIRE(!addBalance(2, 1, Liabilities{0, 0}, 2));

                // Below maximum balance, liabilities
                REQUIRE(addBalance(3, 1, Liabilities{1, 0}, 1));
                REQUIRE(!addBalance(3, 1, Liabilities{1, 0}, 2));
            }
        });
    }
}

TEST_CASE("available balance and limit", "[ledger][liabilities]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& lm = app->getLedgerManager();
    app->start();

    SECTION("account available balance")
    {
        auto checkAvailableBalance = [&](uint32_t initNumSubEntries,
                                         int64_t initBalance,
                                         int64_t initSellingLiabilities) {
            AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
            ae.balance = initBalance;
            ae.numSubEntries = initNumSubEntries;
            if (ae.ext.v() < 1)
            {
                ae.ext.v(1);
            }
            ae.ext.v1().liabilities = Liabilities{0, initSellingLiabilities};

            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account() = ae;

            auto af = std::make_shared<AccountFrame>(le);
            auto availableBalance =
                std::max({int64_t(0), af->getAvailableBalance(lm)});
            REQUIRE(!af->addBalance(-availableBalance - 1, lm));
            REQUIRE(af->addBalance(-availableBalance, lm));
        };

        for_versions_from(10, *app, [&] {
            // Below reserve, no liabilities
            checkAvailableBalance(0, 0, 0);
            checkAvailableBalance(0, lm.getMinBalance(0) - 1, 0);

            // At reserve, no liabilities
            checkAvailableBalance(0, lm.getMinBalance(0), 0);

            // Above reserve, no liabilities
            checkAvailableBalance(0, lm.getMinBalance(0) + 1, 0);
            checkAvailableBalance(0, INT64_MAX, 0);

            // Above reserve, with maximum liabilities
            checkAvailableBalance(0, lm.getMinBalance(0) + 1, 1);
            checkAvailableBalance(0, INT64_MAX,
                                  INT64_MAX - lm.getMinBalance(0));

            // Above reserve, with non-maximum liabilities
            checkAvailableBalance(0, lm.getMinBalance(0) + 2, 1);
            checkAvailableBalance(0, INT64_MAX,
                                  INT64_MAX - lm.getMinBalance(0) - 1);
        });
    }

    SECTION("account available limit")
    {
        auto checkAvailableLimit = [&](uint32_t initNumSubEntries,
                                       int64_t initBalance,
                                       int64_t initBuyingLiabilities) {
            AccountEntry ae = LedgerTestUtils::generateValidAccountEntry();
            ae.balance = initBalance;
            ae.numSubEntries = initNumSubEntries;
            if (ae.ext.v() < 1)
            {
                ae.ext.v(1);
            }
            ae.ext.v1().liabilities = Liabilities{initBuyingLiabilities, 0};

            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account() = ae;

            auto af = std::make_shared<AccountFrame>(le);
            auto availableLimit =
                std::max({int64_t(0), af->getMaxAmountReceive(lm)});
            if (availableLimit < INT64_MAX)
            {
                REQUIRE(!af->addBalance(availableLimit + 1, lm));
            }
            REQUIRE(af->addBalance(availableLimit, lm));
        };

        for_versions_from(10, *app, [&] {
            // Below reserve, no liabilities
            checkAvailableLimit(0, 0, 0);
            checkAvailableLimit(0, lm.getMinBalance(0) - 1, 0);

            // Below reserve, with maximum liabilities
            checkAvailableLimit(0, 0, INT64_MAX);
            checkAvailableLimit(0, lm.getMinBalance(0) - 1,
                                INT64_MAX - lm.getMinBalance(0) + 1);

            // Below reserve, with non-maximum liabilities
            checkAvailableLimit(0, 0, INT64_MAX - 1);
            checkAvailableLimit(0, lm.getMinBalance(0) - 1,
                                INT64_MAX - lm.getMinBalance(0));

            // At reserve, no liabilities
            checkAvailableLimit(0, lm.getMinBalance(0), 0);

            // At reserve, with maximum liabilities
            checkAvailableLimit(0, lm.getMinBalance(0),
                                INT64_MAX - lm.getMinBalance(0));

            // At reserve, with non-maximum liabilities
            checkAvailableLimit(0, lm.getMinBalance(0),
                                INT64_MAX - lm.getMinBalance(0) - 1);

            // Above reserve, no liabilities
            checkAvailableLimit(0, lm.getMinBalance(0) + 1, 0);
            checkAvailableLimit(0, INT64_MAX, 0);

            // Above reserve, with maximum liabilities
            checkAvailableLimit(0, lm.getMinBalance(0) + 1,
                                INT64_MAX - lm.getMinBalance(0) - 1);
            checkAvailableLimit(0, INT64_MAX - 1, 1);

            // Above reserve, with non-maximum liabilities
            checkAvailableLimit(0, lm.getMinBalance(0) + 1,
                                INT64_MAX - lm.getMinBalance(0) - 2);
            checkAvailableLimit(0, INT64_MAX - 2, 1);
        });
    }

    SECTION("trustline available balance")
    {
        auto checkAvailableBalance = [&](int64_t initLimit, int64_t initBalance,
                                         int64_t initSellingLiabilities) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.flags = AUTHORIZED_FLAG;
            tl.balance = initBalance;
            tl.limit = initLimit;
            if (tl.ext.v() < 1)
            {
                tl.ext.v(1);
            }
            tl.ext.v1().liabilities = Liabilities{0, initSellingLiabilities};

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            auto availableBalance =
                std::max({int64_t(0), tf->getAvailableBalance(lm)});
            REQUIRE(!tf->addBalance(-availableBalance - 1, lm));
            REQUIRE(tf->addBalance(-availableBalance, lm));
        };

        for_versions_from(10, *app, [&] {
            // No liabilities
            checkAvailableBalance(1, 0, 0);
            checkAvailableBalance(1, 1, 0);
            checkAvailableBalance(INT64_MAX, INT64_MAX, 0);

            // With maximum liabilities
            checkAvailableBalance(1, 1, 1);
            checkAvailableBalance(2, 2, 2);
            checkAvailableBalance(INT64_MAX, INT64_MAX, INT64_MAX);

            // With non-maximum liabilities
            checkAvailableBalance(2, 2, 1);
            checkAvailableBalance(INT64_MAX, INT64_MAX, 1);
            checkAvailableBalance(INT64_MAX, INT64_MAX, INT64_MAX - 1);
        });
    }

    SECTION("trustline available limit")
    {
        auto checkAvailableLimit = [&](int64_t initLimit, int64_t initBalance,
                                       int64_t initBuyingLiabilities) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.flags = AUTHORIZED_FLAG;
            tl.balance = initBalance;
            tl.limit = initLimit;
            if (tl.ext.v() < 1)
            {
                tl.ext.v(1);
            }
            tl.ext.v1().liabilities = Liabilities{initBuyingLiabilities, 0};

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            auto availableLimit =
                std::max({int64_t(0), tf->getMaxAmountReceive(lm)});
            REQUIRE(!tf->addBalance(availableLimit + 1, lm));
            REQUIRE(tf->addBalance(availableLimit, lm));
        };

        for_versions_from(10, *app, [&] {
            // No liabilities
            checkAvailableLimit(1, 0, 0);
            checkAvailableLimit(1, 1, 0);
            checkAvailableLimit(INT64_MAX, INT64_MAX, 0);

            // With maximum liabilities
            checkAvailableLimit(1, 1, INT64_MAX - 1);
            checkAvailableLimit(INT64_MAX - 1, INT64_MAX - 1, 1);

            // With non-maximum liabilities
            checkAvailableLimit(1, 1, 1);
            checkAvailableLimit(1, 1, INT64_MAX - 2);
            checkAvailableLimit(INT64_MAX - 2, INT64_MAX - 2, 1);
        });
    }

    SECTION("trustline minimum limit")
    {
        auto checkMinimumLimit = [&](int64_t initBalance,
                                     int64_t initBuyingLiabilities) {
            TrustLineEntry tl = LedgerTestUtils::generateValidTrustLineEntry();
            tl.flags = AUTHORIZED_FLAG;
            tl.balance = initBalance;
            tl.limit = INT64_MAX;
            if (tl.ext.v() < 1)
            {
                tl.ext.v(1);
            }
            tl.ext.v1().liabilities = Liabilities{initBuyingLiabilities, 0};

            LedgerEntry le;
            le.data.type(TRUSTLINE);
            le.data.trustLine() = tl;

            auto tf = std::make_shared<TrustFrame>(le);
            auto minimumLimit = tf->getMinimumLimit(lm);
            tf->getTrustLine().limit = minimumLimit;
            REQUIRE(tf->getMaxAmountReceive(lm) == 0);
        };

        for_versions_from(10, *app, [&] {
            // No liabilities
            checkMinimumLimit(0, 0);
            checkMinimumLimit(1, 0);
            checkMinimumLimit(10, 0);
            checkMinimumLimit(INT64_MAX, 0);

            // With maximum liabilities
            checkMinimumLimit(1, INT64_MAX - 1);
            checkMinimumLimit(10, INT64_MAX - 10);
            checkMinimumLimit(INT64_MAX - 1, 1);

            // With non-maximum liabilities
            checkMinimumLimit(1, 1);
            checkMinimumLimit(1, INT64_MAX - 2);
            checkMinimumLimit(INT64_MAX - 2, 1);
        });
    }
}
