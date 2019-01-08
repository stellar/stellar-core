// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
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
        auto addSellingLiabilities = [&](uint32_t initNumSubEntries,
                                         int64_t initBalance,
                                         int64_t initSellingLiabilities,
                                         int64_t deltaLiabilities) {
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            bool res =
                stellar::addSellingLiabilities(header, acc, deltaLiabilities);
            REQUIRE(acc.current().data.account().balance == initBalance);
            REQUIRE(getBuyingLiabilities(header, acc) == initBuyingLiabilities);
            if (res)
            {
                REQUIRE(getSellingLiabilities(header, acc) ==
                        initSellingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(getSellingLiabilities(header, acc) ==
                        initSellingLiabilities);
                REQUIRE(acc.current() == le);
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

                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto header = ltx.loadHeader();
                auto acc = ltx.create(le);
                bool res = stellar::addSellingLiabilities(header, acc,
                                                          deltaLiabilities);
                REQUIRE(acc.current().data.account().balance == initBalance);
                REQUIRE(getBuyingLiabilities(header, acc) == 0);
                if (res)
                {
                    REQUIRE(acc.current().data.account().ext.v() ==
                            ((deltaLiabilities != 0) ? 1 : 0));
                    REQUIRE(getSellingLiabilities(header, acc) ==
                            deltaLiabilities);
                }
                else
                {
                    REQUIRE(getSellingLiabilities(header, acc) == 0);
                    REQUIRE(acc.current() == le);
                }
                return res;
            };

        for_versions_from(10, *app, [&] {
            SECTION("uninitialized liabilities")
            {
                // Uninitialized remains uninitialized after failure
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 1));

                // Uninitialized remains unitialized after success of delta 0
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 0));

                // Uninitialized is initialized after success of delta != 0
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1, 1));
            }

            SECTION("below reserve")
            {
                // Can leave unchanged when account is below reserve
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) - 1, 0,
                                              0));

                // Cannot increase when account is below reserve
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) - 1,
                                               0, 1));

                // No need to test decrease below reserve since that would imply
                // the previous state had excess liabilities
            }

            SECTION("cannot make liabilities negative")
            {
                // No initial liabilities and at maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getLastMinBalance(0), 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 0));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getLastMinBalance(0), 0, -1));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), -1));

                // No initial liabilities and below maximum
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) + 1, 0,
                                              0));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1, 0));
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) + 1,
                                               0, -1));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1, -1));

                // Initial liabilities and at maximum
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) + 1, 1,
                                              -1));
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) + 1,
                                               1, -2));

                // Initial liabilities and below maximum
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) + 2, 1,
                                              -1));
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) + 2,
                                               1, -2));
            }

            SECTION("cannot increase liabilities above balance minus reserve")
            {
                // No initial liabilities and at maximum
                REQUIRE(
                    addSellingLiabilities(0, lm.getLastMinBalance(0), 0, 0));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 0));
                REQUIRE(
                    !addSellingLiabilities(0, lm.getLastMinBalance(0), 0, 1));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 1));

                // No initial liabilities and below maximum
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) + 1, 0,
                                              1));
                REQUIRE(addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1, 1));
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) + 1,
                                               0, 2));
                REQUIRE(!addSellingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1, 2));

                // Initial liabilities and at maximum
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) + 1, 1,
                                              0));
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) + 1,
                                               1, 1));

                // Initial liabilities and below maximum
                REQUIRE(addSellingLiabilities(0, lm.getLastMinBalance(0) + 2, 1,
                                              1));
                REQUIRE(!addSellingLiabilities(0, lm.getLastMinBalance(0) + 2,
                                               1, 2));
            }

            SECTION("limiting values")
            {
                // Can increase to limit but no higher
                REQUIRE(addSellingLiabilities(
                    0, INT64_MAX, 0, INT64_MAX - lm.getLastMinBalance(0)));
                REQUIRE(!addSellingLiabilities(
                    0, INT64_MAX, 0, INT64_MAX - lm.getLastMinBalance(0) + 1));
                REQUIRE(!addSellingLiabilities(
                    0, INT64_MAX, 1, INT64_MAX - lm.getLastMinBalance(0)));

                // Can decrease from limit
                REQUIRE(addSellingLiabilities(
                    0, INT64_MAX, INT64_MAX - lm.getLastMinBalance(0), -1));
                REQUIRE(addSellingLiabilities(
                    0, INT64_MAX, INT64_MAX - lm.getLastMinBalance(0),
                    lm.getLastMinBalance(0) - INT64_MAX));
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            bool res =
                stellar::addBuyingLiabilities(header, acc, deltaLiabilities);
            REQUIRE(acc.current().data.account().balance == initBalance);
            REQUIRE(getSellingLiabilities(header, acc) ==
                    initSellingLiabilities);
            if (res)
            {
                REQUIRE(getBuyingLiabilities(header, acc) ==
                        initBuyingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(getBuyingLiabilities(header, acc) ==
                        initBuyingLiabilities);
                REQUIRE(acc.current() == le);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            bool res =
                stellar::addBuyingLiabilities(header, acc, deltaLiabilities);
            REQUIRE(acc.current().data.account().balance == initBalance);
            REQUIRE(getSellingLiabilities(header, acc) == 0);
            if (res)
            {
                REQUIRE(acc.current().data.account().ext.v() ==
                        ((deltaLiabilities != 0) ? 1 : 0));
                REQUIRE(getBuyingLiabilities(header, acc) == deltaLiabilities);
            }
            else
            {
                REQUIRE(getBuyingLiabilities(header, acc) == 0);
                REQUIRE(acc.current() == le);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("uninitialized liabilities")
            {
                // Uninitialized remains uninitialized after failure
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0),
                    INT64_MAX - (lm.getLastMinBalance(0) - 1)));

                // Uninitialized remains uninitialized after success of delta 0
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 0));

                // Uninitialized is initialized after success of delta != 0
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 1));
            }

            SECTION("below reserve")
            {
                // Can decrease when account is below reserve
                REQUIRE(addBuyingLiabilities(0, lm.getLastMinBalance(0) - 1, 1,
                                             -1));

                // Can leave unchanged when account is below reserve
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0) - 1, 0, 0));
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0) - 1, 1, 0));

                // Can increase when account is below reserve
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0) - 1, 0, 1));
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0) - 1, 1, 1));
            }

            SECTION("cannot make liabilities negative")
            {
                // No initial liabilities
                REQUIRE(addBuyingLiabilities(0, lm.getLastMinBalance(0), 0, 0));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), 0));
                REQUIRE(
                    !addBuyingLiabilities(0, lm.getLastMinBalance(0), 0, -1));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0), -1));

                // Initial liabilities
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0), 1, -1));
                REQUIRE(
                    !addBuyingLiabilities(0, lm.getLastMinBalance(0), 1, -2));
            }

            SECTION("cannot increase liabilities above INT64_MAX minus balance")
            {
                // No initial liabilities, account below reserve
                REQUIRE(addBuyingLiabilities(
                    0, lm.getLastMinBalance(0) - 1, 0,
                    INT64_MAX - (lm.getLastMinBalance(0) - 1)));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) - 1,
                    INT64_MAX - (lm.getLastMinBalance(0) - 1)));
                REQUIRE(!addBuyingLiabilities(
                    0, lm.getLastMinBalance(0) - 1, 0,
                    INT64_MAX - (lm.getLastMinBalance(0) - 2)));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) - 1,
                    INT64_MAX - (lm.getLastMinBalance(0) - 2)));

                // Initial liabilities, account below reserve
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0) - 1, 1,
                                         INT64_MAX - lm.getLastMinBalance(0)));
                REQUIRE(!addBuyingLiabilities(
                    0, lm.getLastMinBalance(0) - 1, 1,
                    INT64_MAX - (lm.getLastMinBalance(0) - 1)));

                // No initial liabilities, account at reserve
                REQUIRE(
                    addBuyingLiabilities(0, lm.getLastMinBalance(0), 0,
                                         INT64_MAX - lm.getLastMinBalance(0)));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0),
                    INT64_MAX - lm.getLastMinBalance(0)));
                REQUIRE(!addBuyingLiabilities(
                    0, lm.getLastMinBalance(0), 0,
                    INT64_MAX - (lm.getLastMinBalance(0) - 1)));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0),
                    INT64_MAX - (lm.getLastMinBalance(0) - 1)));

                // Initial liabilities, account at reserve
                REQUIRE(addBuyingLiabilities(
                    0, lm.getLastMinBalance(0), 1,
                    INT64_MAX - (lm.getLastMinBalance(0) + 1)));
                REQUIRE(
                    !addBuyingLiabilities(0, lm.getLastMinBalance(0), 1,
                                          INT64_MAX - lm.getLastMinBalance(0)));

                // No initial liabilities, account above reserve
                REQUIRE(addBuyingLiabilities(
                    0, lm.getLastMinBalance(0) + 1, 0,
                    INT64_MAX - (lm.getLastMinBalance(0) + 1)));
                REQUIRE(addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1,
                    INT64_MAX - (lm.getLastMinBalance(0) + 1)));
                REQUIRE(
                    !addBuyingLiabilities(0, lm.getLastMinBalance(0) + 1, 0,
                                          INT64_MAX - lm.getLastMinBalance(0)));
                REQUIRE(!addBuyingLiabilitiesUninitialized(
                    0, lm.getLastMinBalance(0) + 1,
                    INT64_MAX - lm.getLastMinBalance(0)));

                // Initial liabilities, account above reserve
                REQUIRE(addBuyingLiabilities(
                    0, lm.getLastMinBalance(0) + 1, 1,
                    INT64_MAX - (lm.getLastMinBalance(0) + 2)));
                REQUIRE(!addBuyingLiabilities(
                    0, lm.getLastMinBalance(0) + 1, 1,
                    INT64_MAX - (lm.getLastMinBalance(0) + 1)));
            }

            SECTION("limiting values")
            {
                REQUIRE(!addBuyingLiabilities(0, INT64_MAX, 0, 1));
                REQUIRE(addBuyingLiabilities(0, INT64_MAX - 1, 0, 1));

                REQUIRE(!addBuyingLiabilities(
                    0, lm.getLastMinBalance(0),
                    INT64_MAX - lm.getLastMinBalance(0), 1));
                REQUIRE(addBuyingLiabilities(
                    0, lm.getLastMinBalance(0),
                    INT64_MAX - lm.getLastMinBalance(0) - 1, 1));

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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            bool res =
                stellar::addSellingLiabilities(header, trust, deltaLiabilities);
            REQUIRE(trust.current().data.trustLine().balance == initBalance);
            REQUIRE(getBuyingLiabilities(header, trust) ==
                    initBuyingLiabilities);
            if (res)
            {
                REQUIRE(getSellingLiabilities(header, trust) ==
                        initSellingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(getSellingLiabilities(header, trust) ==
                        initSellingLiabilities);
                REQUIRE(trust.current() == le);
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

                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto header = ltx.loadHeader();
                auto trust = ltx.create(le);
                bool res = stellar::addSellingLiabilities(header, trust,
                                                          deltaLiabilities);
                REQUIRE(trust.current().data.trustLine().balance ==
                        initBalance);
                REQUIRE(getBuyingLiabilities(header, trust) == 0);
                if (res)
                {
                    REQUIRE(getSellingLiabilities(header, trust) ==
                            deltaLiabilities);
                }
                else
                {
                    REQUIRE(getSellingLiabilities(header, trust) == 0);
                    REQUIRE(trust.current() == le);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            bool res =
                stellar::addBuyingLiabilities(header, trust, deltaLiabilities);
            REQUIRE(trust.current().data.trustLine().balance == initBalance);
            REQUIRE(getSellingLiabilities(header, trust) ==
                    initSellingLiabilities);
            if (res)
            {
                REQUIRE(getBuyingLiabilities(header, trust) ==
                        initBuyingLiabilities + deltaLiabilities);
            }
            else
            {
                REQUIRE(getBuyingLiabilities(header, trust) ==
                        initBuyingLiabilities);
                REQUIRE(trust.current() == le);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            bool res =
                stellar::addBuyingLiabilities(header, trust, deltaLiabilities);
            REQUIRE(trust.current().data.trustLine().balance == initBalance);
            REQUIRE(getSellingLiabilities(header, trust) == 0);
            if (res)
            {
                REQUIRE(getBuyingLiabilities(header, trust) ==
                        deltaLiabilities);
            }
            else
            {
                REQUIRE(getBuyingLiabilities(header, trust) == 0);
                REQUIRE(trust.current() == le);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            bool res = stellar::addBalance(header, acc, deltaBalance);
            REQUIRE(getSellingLiabilities(header, acc) ==
                    initLiabilities.selling);
            REQUIRE(getBuyingLiabilities(header, acc) ==
                    initLiabilities.buying);
            if (res)
            {
                REQUIRE(acc.current().data.account().balance ==
                        initBalance + deltaBalance);
            }
            else
            {
                REQUIRE(acc.current() == le);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("can increase balance from below minimum")
            {
                // Balance can remain unchanged below reserve
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) - 1,
                                   Liabilities{0, 0}, 0));

                // Balance starts and ends below reserve
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) - 2,
                                   Liabilities{0, 0}, 1));

                // Balance starts below reserve and ends at reserve
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) - 1,
                                   Liabilities{0, 0}, 1));

                // Balance starts below reserve and ends above reserve
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) - 1,
                                   Liabilities{0, 0}, 2));
            }

            SECTION("cannot decrease balance below reserve plus selling "
                    "liabilities")
            {
                // Below minimum balance, no liabilities
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) - 1,
                                   Liabilities{0, 0}, 0));
                REQUIRE(!addBalance(0, lm.getLastMinBalance(0) - 1,
                                    Liabilities{0, 0}, -1));

                // Minimum balance, no liabilities
                REQUIRE(addBalance(0, lm.getLastMinBalance(0),
                                   Liabilities{0, 0}, 0));
                REQUIRE(!addBalance(0, lm.getLastMinBalance(0),
                                    Liabilities{0, 0}, -1));

                // Above minimum balance, no liabilities
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) + 1,
                                   Liabilities{0, 0}, -1));
                REQUIRE(!addBalance(0, lm.getLastMinBalance(0) + 1,
                                    Liabilities{0, 0}, -2));

                // Above minimum balance, with liabilities
                REQUIRE(addBalance(0, lm.getLastMinBalance(0) + 1,
                                   Liabilities{0, 1}, 0));
                REQUIRE(!addBalance(0, lm.getLastMinBalance(0) + 1,
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            bool res = stellar::addNumEntries(header, acc, deltaNumSubEntries);
            REQUIRE(getSellingLiabilities(header, acc) ==
                    initSellingLiabilities);
            REQUIRE(getBuyingLiabilities(header, acc) == initBuyingLiabilities);
            REQUIRE(acc.current().data.account().balance == initBalance);
            if (res)
            {
                if (deltaNumSubEntries > 0)
                {
                    REQUIRE(getAvailableBalance(header, acc) >= 0);
                }
                REQUIRE(acc.current().data.account().numSubEntries ==
                        initNumSubEntries + deltaNumSubEntries);
            }
            else
            {
                REQUIRE(acc.current() == le);
            }
            return res;
        };

        for_versions_from(10, *app, [&] {
            SECTION("can decrease sub entries when below min balance")
            {
                // Below reserve and below new reserve
                REQUIRE(addSubEntries(1, 0, 0, -1));
                REQUIRE(addSubEntries(1, lm.getLastMinBalance(0) - 1, 0, -1));

                // Below reserve but at new reserve
                REQUIRE(addSubEntries(1, lm.getLastMinBalance(0), 0, -1));

                // Below reserve but above new reserve
                REQUIRE(addSubEntries(1, lm.getLastMinBalance(1) - 1, 0, -1));
                REQUIRE(addSubEntries(1, lm.getLastMinBalance(0) + 1, 0, -1));
            }

            SECTION("cannot add sub entry without sufficient balance")
            {
                // Below reserve, no liabilities
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(0) - 1, 0, 1));

                // At reserve, no liabilities
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(0), 0, 1));

                // Above reserve but below new reserve, no liabilities
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(0) + 1, 0, 1));
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(1) - 1, 0, 1));

                // Above reserve but below new reserve, with liabilities
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(0) + 2, 1, 1));
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(1), 1, 1));

                // Above reserve but at new reserve, no liabilities
                REQUIRE(addSubEntries(0, lm.getLastMinBalance(1), 0, 1));

                // Above reserve but at new reserve, with liabilities
                REQUIRE(addSubEntries(0, lm.getLastMinBalance(1) + 1, 1, 1));

                // Above reserve and above new reserve, no liabilities
                REQUIRE(addSubEntries(0, lm.getLastMinBalance(1) + 1, 0, 1));

                // Above reserve and above new reserve, with liabilities
                REQUIRE(addSubEntries(0, lm.getLastMinBalance(1) + 1, 1, 1));
                REQUIRE(!addSubEntries(0, lm.getLastMinBalance(1) + 1, 2, 1));
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            bool res = stellar::addBalance(header, trust, deltaBalance);
            REQUIRE(getSellingLiabilities(header, trust) ==
                    initLiabilities.selling);
            REQUIRE(getBuyingLiabilities(header, trust) ==
                    initLiabilities.buying);
            if (res)
            {
                REQUIRE(trust.current().data.trustLine().balance ==
                        initBalance + deltaBalance);
            }
            else
            {
                REQUIRE(trust.current() == le);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            auto availableBalance =
                std::max({int64_t(0), getAvailableBalance(header, acc)});
            REQUIRE(!stellar::addBalance(header, acc, -availableBalance - 1));
            REQUIRE(stellar::addBalance(header, acc, -availableBalance));
        };

        for_versions_from(10, *app, [&] {
            // Below reserve, no liabilities
            checkAvailableBalance(0, 0, 0);
            checkAvailableBalance(0, lm.getLastMinBalance(0) - 1, 0);

            // At reserve, no liabilities
            checkAvailableBalance(0, lm.getLastMinBalance(0), 0);

            // Above reserve, no liabilities
            checkAvailableBalance(0, lm.getLastMinBalance(0) + 1, 0);
            checkAvailableBalance(0, INT64_MAX, 0);

            // Above reserve, with maximum liabilities
            checkAvailableBalance(0, lm.getLastMinBalance(0) + 1, 1);
            checkAvailableBalance(0, INT64_MAX,
                                  INT64_MAX - lm.getLastMinBalance(0));

            // Above reserve, with non-maximum liabilities
            checkAvailableBalance(0, lm.getLastMinBalance(0) + 2, 1);
            checkAvailableBalance(0, INT64_MAX,
                                  INT64_MAX - lm.getLastMinBalance(0) - 1);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto acc = ltx.create(le);
            auto availableLimit =
                std::max({int64_t(0), getMaxAmountReceive(header, acc)});
            if (availableLimit < INT64_MAX)
            {
                REQUIRE(!stellar::addBalance(header, acc, availableLimit + 1));
            }
            REQUIRE(stellar::addBalance(header, acc, availableLimit));
        };

        for_versions_from(10, *app, [&] {
            // Below reserve, no liabilities
            checkAvailableLimit(0, 0, 0);
            checkAvailableLimit(0, lm.getLastMinBalance(0) - 1, 0);

            // Below reserve, with maximum liabilities
            checkAvailableLimit(0, 0, INT64_MAX);
            checkAvailableLimit(0, lm.getLastMinBalance(0) - 1,
                                INT64_MAX - lm.getLastMinBalance(0) + 1);

            // Below reserve, with non-maximum liabilities
            checkAvailableLimit(0, 0, INT64_MAX - 1);
            checkAvailableLimit(0, lm.getLastMinBalance(0) - 1,
                                INT64_MAX - lm.getLastMinBalance(0));

            // At reserve, no liabilities
            checkAvailableLimit(0, lm.getLastMinBalance(0), 0);

            // At reserve, with maximum liabilities
            checkAvailableLimit(0, lm.getLastMinBalance(0),
                                INT64_MAX - lm.getLastMinBalance(0));

            // At reserve, with non-maximum liabilities
            checkAvailableLimit(0, lm.getLastMinBalance(0),
                                INT64_MAX - lm.getLastMinBalance(0) - 1);

            // Above reserve, no liabilities
            checkAvailableLimit(0, lm.getLastMinBalance(0) + 1, 0);
            checkAvailableLimit(0, INT64_MAX, 0);

            // Above reserve, with maximum liabilities
            checkAvailableLimit(0, lm.getLastMinBalance(0) + 1,
                                INT64_MAX - lm.getLastMinBalance(0) - 1);
            checkAvailableLimit(0, INT64_MAX - 1, 1);

            // Above reserve, with non-maximum liabilities
            checkAvailableLimit(0, lm.getLastMinBalance(0) + 1,
                                INT64_MAX - lm.getLastMinBalance(0) - 2);
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            auto availableBalance =
                std::max({int64_t(0), getAvailableBalance(header, trust)});
            REQUIRE(!stellar::addBalance(header, trust, -availableBalance - 1));
            REQUIRE(stellar::addBalance(header, trust, -availableBalance));
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            auto availableLimit =
                std::max({int64_t(0), getMaxAmountReceive(header, trust)});
            if (availableLimit < INT64_MAX)
            {
                REQUIRE(
                    !stellar::addBalance(header, trust, availableLimit + 1));
            }
            REQUIRE(stellar::addBalance(header, trust, availableLimit));
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

            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            auto trust = ltx.create(le);
            trust.current().data.trustLine().limit =
                getMinimumLimit(header, trust);
            REQUIRE(getMaxAmountReceive(header, trust) == 0);
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
