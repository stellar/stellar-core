// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/uint128_t.h"
#include "transactions/OfferExchange.h"

using namespace stellar;

TEST_CASE("Exchange", "[exchange]")
{
    enum ReducedCheckV2
    {
        REDUCED_CHECK_V2_RELAXED,
        REDUCED_CHECK_V2_STRICT
    };

    auto compare = [](ExchangeResult const& x, ExchangeResult const& y) {
        REQUIRE(x.type() == ExchangeResultType::NORMAL);
        REQUIRE(x.reduced == y.reduced);
        REQUIRE(x.numWheatReceived == y.numWheatReceived);
        REQUIRE(x.numSheepSend == y.numSheepSend);
    };
    auto validateV2 =
        [&compare](int64_t wheatToReceive, Price price, int64_t maxWheatReceive,
                   int64_t maxSheepSend, ExchangeResult const& expected,
                   ReducedCheckV2 reducedCheck = REDUCED_CHECK_V2_STRICT) {
            auto actualV2 = exchangeV2(wheatToReceive, price, maxWheatReceive,
                                       maxSheepSend);
            compare(actualV2, expected);
            REQUIRE(uint128_t{actualV2.numWheatReceived} * uint128_t{price.n} <=
                    uint128_t{expected.numSheepSend} * uint128_t{price.d});
            REQUIRE(actualV2.numSheepSend <= maxSheepSend);
            if (reducedCheck == REDUCED_CHECK_V2_RELAXED)
            {
                REQUIRE(actualV2.numWheatReceived <= wheatToReceive);
            }
            else
            {
                if (actualV2.reduced)
                {
                    REQUIRE(actualV2.numWheatReceived < wheatToReceive);
                }
                else
                {
                    REQUIRE(actualV2.numWheatReceived == wheatToReceive);
                }
            }
        };
    auto validateV3 = [&compare](int64_t wheatToReceive, Price price,
                                 int64_t maxWheatReceive, int64_t maxSheepSend,
                                 ExchangeResult const& expected) {
        auto actualV3 =
            exchangeV3(wheatToReceive, price, maxWheatReceive, maxSheepSend);
        compare(actualV3, expected);
        REQUIRE(uint128_t{actualV3.numWheatReceived} * uint128_t{price.n} <=
                uint128_t{expected.numSheepSend} * uint128_t{price.d});
        REQUIRE(actualV3.numSheepSend <= maxSheepSend);
        if (actualV3.reduced)
        {
            REQUIRE(actualV3.numWheatReceived < wheatToReceive);
        }
        else
        {
            REQUIRE(actualV3.numWheatReceived == wheatToReceive);
        }
    };
    auto validate = [&validateV2, &validateV3](
                        int64_t wheatToReceive, Price price,
                        int64_t maxWheatReceive, int64_t maxSheepSend,
                        ExchangeResult const& expected,
                        ReducedCheckV2 reducedCheck = REDUCED_CHECK_V2_STRICT) {
        validateV2(wheatToReceive, price, maxWheatReceive, maxSheepSend,
                   expected, reducedCheck);
        validateV3(wheatToReceive, price, maxWheatReceive, maxSheepSend,
                   expected);
    };

    SECTION("normal prices")
    {
        SECTION("no limits")
        {
            SECTION("1000")
            {
                validate(1000, Price{3, 2}, INT64_MAX, INT64_MAX,
                         {1000, 1500, false});
                validate(1000, Price{1, 1}, INT64_MAX, INT64_MAX,
                         {1000, 1000, false});
                validateV2(1000, Price{2, 3}, INT64_MAX, INT64_MAX,
                           {999, 666, false}, REDUCED_CHECK_V2_RELAXED);
                validateV3(1000, Price{2, 3}, INT64_MAX, INT64_MAX,
                           {1000, 667, false});
            }

            SECTION("999")
            {
                validateV2(999, Price{3, 2}, INT64_MAX, INT64_MAX,
                           {998, 1498, false}, REDUCED_CHECK_V2_RELAXED);
                validateV3(999, Price{3, 2}, INT64_MAX, INT64_MAX,
                           {999, 1499, false});
                validate(999, Price{1, 1}, INT64_MAX, INT64_MAX,
                         {999, 999, false});
                validate(999, Price{2, 3}, INT64_MAX, INT64_MAX,
                         {999, 666, false});
            }

            SECTION("1")
            {
                REQUIRE(
                    exchangeV2(0, Price{3, 2}, INT64_MAX, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
                REQUIRE(
                    exchangeV3(0, Price{3, 2}, INT64_MAX, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
                validate(1, Price{1, 1}, INT64_MAX, INT64_MAX, {1, 1, false});
                REQUIRE(
                    exchangeV2(1, Price{2, 3}, INT64_MAX, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
                validateV3(1, Price{2, 3}, INT64_MAX, INT64_MAX, {1, 1, false});
            }

            SECTION("0")
            {
                REQUIRE(
                    exchangeV2(0, Price{3, 2}, INT64_MAX, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
                REQUIRE(
                    exchangeV2(0, Price{1, 1}, INT64_MAX, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
                REQUIRE(
                    exchangeV2(0, Price{2, 3}, INT64_MAX, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
            }
        }

        SECTION("send limits")
        {
            SECTION("1000 limited to 500")
            {
                validate(1000, Price{3, 2}, INT64_MAX, 750, {500, 750, true});
                validate(1000, Price{1, 1}, INT64_MAX, 500, {500, 500, true});
                validate(1000, Price{2, 3}, INT64_MAX, 333, {499, 333, true});
            }

            SECTION("999 limited to 499")
            {
                validate(999, Price{3, 2}, INT64_MAX, 749, {499, 749, true});
                validate(999, Price{1, 1}, INT64_MAX, 499, {499, 499, true});
                validate(999, Price{2, 3}, INT64_MAX, 333, {499, 333, true});
            }

            SECTION("20 limited to 10")
            {
                validate(20, Price{3, 2}, INT64_MAX, 15, {10, 15, true});
                validate(20, Price{1, 1}, INT64_MAX, 10, {10, 10, true});
                validate(20, Price{2, 3}, INT64_MAX, 7, {10, 7, true});
            }

            SECTION("2 limited to 1")
            {
                validate(2, Price{3, 2}, INT64_MAX, 2, {1, 2, true});
                validate(2, Price{1, 1}, INT64_MAX, 1, {1, 1, true});
                validateV2(2, Price{2, 3}, INT64_MAX, 1, {1, 1, false},
                           REDUCED_CHECK_V2_RELAXED);
                validateV3(2, Price{2, 3}, INT64_MAX, 1, {1, 1, true});
            }
        }

        SECTION("receive limits")
        {
            SECTION("1000 limited to 500")
            {
                validate(1000, Price{3, 2}, 500, INT64_MAX, {500, 750, true});
                validate(1000, Price{1, 1}, 500, INT64_MAX, {500, 500, true});
                validateV2(1000, Price{2, 3}, 500, INT64_MAX, {499, 333, true});
                validateV3(1000, Price{2, 3}, 500, INT64_MAX, {500, 334, true});
            }

            SECTION("999 limited to 499")
            {
                validateV2(999, Price{3, 2}, 499, INT64_MAX, {498, 748, true});
                validateV3(999, Price{3, 2}, 499, INT64_MAX, {499, 749, true});
                validate(999, Price{1, 1}, 499, INT64_MAX, {499, 499, true});
                validateV2(999, Price{2, 3}, 499, INT64_MAX, {498, 332, true});
                validateV3(999, Price{2, 3}, 499, INT64_MAX, {499, 333, true});
            }

            SECTION("20 limited to 10")
            {
                validate(20, Price{3, 2}, 10, INT64_MAX, {10, 15, true});
                validate(20, Price{1, 1}, 10, INT64_MAX, {10, 10, true});
                validateV2(20, Price{2, 3}, 10, INT64_MAX, {9, 6, true});
                validateV3(20, Price{2, 3}, 10, INT64_MAX, {10, 7, true});
            }

            SECTION("2 limited to 1")
            {
                REQUIRE(exchangeV2(2, Price{3, 2}, 1, INT64_MAX).type() ==
                        ExchangeResultType::REDUCED_TO_ZERO);
                validateV3(2, Price{3, 2}, 1, INT64_MAX, {1, 2, true});
                validate(2, Price{1, 1}, 1, INT64_MAX, {1, 1, true});
                REQUIRE(exchangeV2(2, Price{2, 3}, 1, INT64_MAX).type() ==
                        ExchangeResultType::REDUCED_TO_ZERO);
                validateV3(2, Price{2, 3}, 1, INT64_MAX, {1, 1, true});
            }
        }
    }

    SECTION("extra big prices")
    {
        SECTION("no limits")
        {
            validate(1000, Price{INT32_MAX, 1}, INT64_MAX, INT64_MAX,
                     {1000, 1000ull * INT32_MAX, false});
            validate(999, Price{INT32_MAX, 1}, INT64_MAX, INT64_MAX,
                     {999, 999ull * INT32_MAX, false});
            validate(1, Price{INT32_MAX, 1}, INT64_MAX, INT64_MAX,
                     {1, INT32_MAX, false});
            REQUIRE(exchangeV2(2, Price{2, 3}, 1, INT64_MAX).type() ==
                    ExchangeResultType::REDUCED_TO_ZERO);
            validateV3(2, Price{2, 3}, 1, INT64_MAX, {1, 1, true});
        }

        SECTION("send limits")
        {
            SECTION("750")
            {
                REQUIRE(exchangeV2(1000, Price{INT32_MAX, 1}, INT64_MAX, 750)
                            .type() == ExchangeResultType::REDUCED_TO_ZERO);
                REQUIRE(exchangeV3(1000, Price{INT32_MAX, 1}, INT64_MAX, 750)
                            .type() == ExchangeResultType::REDUCED_TO_ZERO);
                REQUIRE(exchangeV2(999, Price{INT32_MAX, 1}, INT64_MAX, 750)
                            .type() == ExchangeResultType::REDUCED_TO_ZERO);
                REQUIRE(exchangeV3(999, Price{INT32_MAX, 1}, INT64_MAX, 750)
                            .type() == ExchangeResultType::REDUCED_TO_ZERO);
                REQUIRE(
                    exchangeV2(1, Price{INT32_MAX, 1}, INT64_MAX, 750).type() ==
                    ExchangeResultType::REDUCED_TO_ZERO);
                REQUIRE(
                    exchangeV3(1, Price{INT32_MAX, 1}, INT64_MAX, 750).type() ==
                    ExchangeResultType::REDUCED_TO_ZERO);
                REQUIRE(
                    exchangeV2(0, Price{INT32_MAX, 1}, INT64_MAX, 750).type() ==
                    ExchangeResultType::BOGUS);
                REQUIRE(
                    exchangeV3(0, Price{INT32_MAX, 1}, INT64_MAX, 750).type() ==
                    ExchangeResultType::BOGUS);
            }

            SECTION("INT32_MAX")
            {
                validate(1000, Price{INT32_MAX, 1}, INT64_MAX, INT32_MAX,
                         {1, INT32_MAX, true});
                validate(999, Price{INT32_MAX, 1}, INT64_MAX, INT32_MAX,
                         {1, INT32_MAX, true});
                validate(1, Price{INT32_MAX, 1}, INT64_MAX, INT32_MAX,
                         {1, INT32_MAX, false});
                REQUIRE(exchangeV2(0, Price{INT32_MAX, 1}, INT64_MAX, INT32_MAX)
                            .type() == ExchangeResultType::BOGUS);
                REQUIRE(exchangeV3(0, Price{INT32_MAX, 1}, INT64_MAX, INT32_MAX)
                            .type() == ExchangeResultType::BOGUS);
            }

            SECTION("750 * INT32_MAX")
            {
                validate(1000, Price{INT32_MAX, 1}, INT64_MAX,
                         750ull * INT32_MAX, {750, 750ull * INT32_MAX, true});
                validate(999, Price{INT32_MAX, 1}, INT64_MAX,
                         750ull * INT32_MAX, {750, 750ull * INT32_MAX, true});
                validate(1, Price{INT32_MAX, 1}, INT64_MAX, 750ull * INT32_MAX,
                         {1, INT32_MAX, false});
                REQUIRE(exchangeV2(0, Price{INT32_MAX, 1}, INT64_MAX,
                                   750ull * INT32_MAX)
                            .type() == ExchangeResultType::BOGUS);
                REQUIRE(exchangeV3(0, Price{INT32_MAX, 1}, INT64_MAX,
                                   750ull * INT32_MAX)
                            .type() == ExchangeResultType::BOGUS);
            }
        }

        SECTION("receive limits")
        {
            SECTION("750")
            {
                validate(1000, Price{INT32_MAX, 1}, 750, INT64_MAX,
                         {750, 750ull * INT32_MAX, true});
                validate(999, Price{INT32_MAX, 1}, 750, INT64_MAX,
                         {750, 750ull * INT32_MAX, true});
                validate(1, Price{INT32_MAX, 1}, 750, INT64_MAX,
                         {1, INT32_MAX, false});
                REQUIRE(
                    exchangeV2(0, Price{INT32_MAX, 1}, 750, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
                REQUIRE(
                    exchangeV3(0, Price{INT32_MAX, 1}, 750, INT64_MAX).type() ==
                    ExchangeResultType::BOGUS);
            }

            SECTION("INT32_MAX")
            {
                validate(1000, Price{INT32_MAX, 1}, INT32_MAX, INT64_MAX,
                         {1000, 1000ull * INT32_MAX, false});
                validate(999, Price{INT32_MAX, 1}, INT32_MAX, INT64_MAX,
                         {999, 999ull * INT32_MAX, false});
                validate(1, Price{INT32_MAX, 1}, INT32_MAX, INT64_MAX,
                         {1, INT32_MAX, false});
                REQUIRE(exchangeV2(0, Price{INT32_MAX, 1}, INT32_MAX, INT64_MAX)
                            .type() == ExchangeResultType::BOGUS);
                REQUIRE(exchangeV3(0, Price{INT32_MAX, 1}, INT32_MAX, INT64_MAX)
                            .type() == ExchangeResultType::BOGUS);
            }
        }
    }

    SECTION("extra small prices")
    {
        SECTION("no limits")
        {
            validate(1000ull * INT32_MAX, Price{1, INT32_MAX}, INT64_MAX,
                     INT64_MAX, {1000ull * INT32_MAX, 1000, false});
            validate(999ull * INT32_MAX, Price{1, INT32_MAX}, INT64_MAX,
                     INT64_MAX, {999ull * INT32_MAX, 999, false});
            validate(INT32_MAX, Price{1, INT32_MAX}, INT64_MAX, INT64_MAX,
                     {INT32_MAX, 1, false});
            REQUIRE(exchangeV2(0, Price{1, INT32_MAX}, INT64_MAX, INT64_MAX)
                        .type() == ExchangeResultType::BOGUS);
            REQUIRE(exchangeV3(0, Price{1, INT32_MAX}, INT64_MAX, INT64_MAX)
                        .type() == ExchangeResultType::BOGUS);
        }

        SECTION("send limits")
        {
            SECTION("750")
            {
                validate(1000ull * INT32_MAX, Price{1, INT32_MAX}, INT64_MAX,
                         750, {750ull * INT32_MAX, 750, true});
                validate(999ull * INT32_MAX, Price{1, INT32_MAX}, INT64_MAX,
                         750, {750ull * INT32_MAX, 750, true});
                validate(INT32_MAX, Price{1, INT32_MAX}, INT64_MAX, 750,
                         {INT32_MAX, 1, false});
                REQUIRE(
                    exchangeV2(0, Price{1, INT32_MAX}, INT64_MAX, 750).type() ==
                    ExchangeResultType::BOGUS);
                REQUIRE(
                    exchangeV3(0, Price{1, INT32_MAX}, INT64_MAX, 750).type() ==
                    ExchangeResultType::BOGUS);
            }

            SECTION("INT32_MAX")
            {
                validate(1000ull * INT32_MAX, Price{1, INT32_MAX}, INT64_MAX,
                         INT32_MAX, {1000ull * INT32_MAX, 1000, false});
                validate(999ull * INT32_MAX, Price{1, INT32_MAX}, INT64_MAX,
                         INT32_MAX, {999ull * INT32_MAX, 999, false});
                validate(INT32_MAX, Price{1, INT32_MAX}, INT64_MAX, INT32_MAX,
                         {INT32_MAX, 1, false});
                REQUIRE(exchangeV2(0, Price{1, INT32_MAX}, INT64_MAX, INT32_MAX)
                            .type() == ExchangeResultType::BOGUS);
                REQUIRE(exchangeV3(0, Price{1, INT32_MAX}, INT64_MAX, INT32_MAX)
                            .type() == ExchangeResultType::BOGUS);
            }
        }

        SECTION("receive limits")
        {
            SECTION("750")
            {
                REQUIRE(exchangeV2(1000ull * INT32_MAX, Price{1, INT32_MAX},
                                   750, INT64_MAX)
                            .type() == ExchangeResultType::REDUCED_TO_ZERO);
                validateV3(1000ull * INT32_MAX, Price{1, INT32_MAX}, 750,
                           INT64_MAX, {750, 1, true});
                REQUIRE(exchangeV2(999ull * INT32_MAX, Price{1, INT32_MAX}, 750,
                                   INT64_MAX)
                            .type() == ExchangeResultType::REDUCED_TO_ZERO);
                validateV3(999ull * INT32_MAX, Price{1, INT32_MAX}, 750,
                           INT64_MAX, {750, 1, true});
                REQUIRE(
                    exchangeV2(INT32_MAX, Price{1, INT32_MAX}, 750, INT64_MAX)
                        .type() == ExchangeResultType::REDUCED_TO_ZERO);
                validateV3(INT32_MAX, Price{1, INT32_MAX}, 750, INT64_MAX,
                           {750, 1, true});
                REQUIRE(exchangeV2(750, Price{1, INT32_MAX}, 750, INT64_MAX)
                            .type() == ExchangeResultType::BOGUS);
                validateV3(750, Price{1, INT32_MAX}, 750, INT64_MAX,
                           {750, 1, false});
            }

            SECTION("INT32_MAX")
            {
                validate(1000ull * INT32_MAX, Price{1, INT32_MAX},
                         750ull * INT32_MAX, INT64_MAX,
                         {750ull * INT32_MAX, 750, true});
                validate(999ull * INT32_MAX, Price{1, INT32_MAX},
                         750ull * INT32_MAX, INT64_MAX,
                         {750ull * INT32_MAX, 750, true});
                validate(INT32_MAX, Price{1, INT32_MAX}, 750ull * INT32_MAX,
                         INT64_MAX, {INT32_MAX, 1, false});
                REQUIRE(exchangeV2(750, Price{1, INT32_MAX}, 750ull * INT32_MAX,
                                   INT64_MAX)
                            .type() == ExchangeResultType::BOGUS);
                validateV3(750, Price{1, INT32_MAX}, 750ull * INT32_MAX,
                           INT64_MAX, {750, 1, false});
            }

            SECTION("750 * INT32_MAX")
            {
                validate(1000ull * INT32_MAX, Price{1, INT32_MAX},
                         750ull * INT32_MAX, INT64_MAX,
                         {750ull * INT32_MAX, 750, true});
                validate(999ull * INT32_MAX, Price{1, INT32_MAX},
                         750ull * INT32_MAX, INT64_MAX,
                         {750ull * INT32_MAX, 750, true});
                validate(INT32_MAX, Price{1, INT32_MAX}, 750ull * INT32_MAX,
                         INT64_MAX, {INT32_MAX, 1, false});
                REQUIRE(exchangeV2(750, Price{1, INT32_MAX}, 750ull * INT32_MAX,
                                   INT64_MAX)
                            .type() == ExchangeResultType::BOGUS);
                validateV3(750, Price{1, INT32_MAX}, 750ull * INT32_MAX,
                           INT64_MAX, {750, 1, false});
            }
        }
    }

    SECTION("exchange with big limits")
    {
        SECTION("INT32_MAX send")
        {
            validate(INT32_MAX, Price{3, 2}, INT64_MAX, INT32_MAX,
                     {1431655764, INT32_MAX, true});
            validate(INT32_MAX, Price{1, 1}, INT64_MAX, INT32_MAX,
                     {INT32_MAX, INT32_MAX, false});
            validateV2(INT32_MAX, Price{2, 3}, INT64_MAX, INT32_MAX,
                       {INT32_MAX - 1, 1431655764, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT32_MAX, Price{2, 3}, INT64_MAX, INT32_MAX,
                       {INT32_MAX, 1431655765, false});
            validate(INT32_MAX, Price{1, INT32_MAX}, INT64_MAX, INT32_MAX,
                     {INT32_MAX, 1, false});
            validate(INT32_MAX, Price{INT32_MAX, 1}, INT64_MAX, INT32_MAX,
                     {1, INT32_MAX, true});
            validate(INT32_MAX, Price{INT32_MAX, INT32_MAX}, INT64_MAX,
                     INT32_MAX, {INT32_MAX, INT32_MAX, false});
        }

        SECTION("INT32_MAX receive")
        {
            validateV2(INT32_MAX, Price{3, 2}, INT32_MAX, INT64_MAX,
                       {INT32_MAX - 1, 3221225470, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT32_MAX, Price{3, 2}, INT32_MAX, INT64_MAX,
                       {INT32_MAX, 3221225471, false});
            validate(INT32_MAX, Price{1, 1}, INT32_MAX, INT64_MAX,
                     {INT32_MAX, INT32_MAX, false});
            validateV2(INT32_MAX, Price{2, 3}, INT32_MAX, INT64_MAX,
                       {INT32_MAX - 1, 1431655764, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT32_MAX, Price{2, 3}, INT32_MAX, INT64_MAX,
                       {INT32_MAX, 1431655765, false});
            validate(INT32_MAX, Price{1, INT32_MAX}, INT32_MAX, INT64_MAX,
                     {INT32_MAX, 1, false});
            validate(INT32_MAX, Price{INT32_MAX, 1}, INT32_MAX, INT64_MAX,
                     {INT32_MAX, 4611686014132420609, false});
            validate(INT32_MAX, Price{INT32_MAX, INT32_MAX}, INT32_MAX,
                     INT64_MAX, {INT32_MAX, INT32_MAX, false});
        }

        SECTION("INT64_MAX")
        {
            validateV2(INT64_MAX, Price{3, 2}, INT64_MAX, INT64_MAX,
                       {6148914691236517204, INT64_MAX, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT64_MAX, Price{3, 2}, INT64_MAX, INT64_MAX,
                       {6148914691236517204, INT64_MAX, true});
            validate(INT64_MAX, Price{1, 1}, INT64_MAX, INT64_MAX,
                     {INT64_MAX, INT64_MAX, false});
            validateV2(INT64_MAX, Price{2, 3}, INT64_MAX, INT64_MAX,
                       {INT64_MAX - 1, 6148914691236517204, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT64_MAX, Price{2, 3}, INT64_MAX, INT64_MAX,
                       {INT64_MAX, 6148914691236517205, false});
            validateV2(INT64_MAX, Price{1, INT32_MAX}, INT64_MAX, INT64_MAX,
                       {INT64_MAX - 1, 4294967298, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT64_MAX, Price{1, INT32_MAX}, INT64_MAX, INT64_MAX,
                       {INT64_MAX, 4294967299, false});
            validateV2(INT64_MAX, Price{INT32_MAX, 1}, INT64_MAX, INT64_MAX,
                       {4294967298, INT64_MAX, false},
                       REDUCED_CHECK_V2_RELAXED);
            validateV3(INT64_MAX, Price{INT32_MAX, 1}, INT64_MAX, INT64_MAX,
                       {4294967298, INT64_MAX, true});
            validate(INT64_MAX, Price{INT32_MAX, INT32_MAX}, INT64_MAX,
                     INT64_MAX, {INT64_MAX, INT64_MAX, false});
        }
    }
}

TEST_CASE("ExchangeV10", "[exchange]")
{
    SECTION("Limited by maxWheatSend and maxSheepSend")
    {
        auto checkExchangeV10 = [](Price const& p, int64_t maxWheatSend,
                                   int64_t maxSheepSend, int64_t wheatReceive,
                                   int64_t sheepSend) {
            auto res = exchangeV10(p, maxWheatSend, INT64_MAX, maxSheepSend,
                                   INT64_MAX, false);
            REQUIRE(res.wheatStays ==
                    (maxWheatSend * p.n > maxSheepSend * p.d));
            REQUIRE(res.numWheatReceived == wheatReceive);
            REQUIRE(res.numSheepSend == sheepSend);
            if (res.wheatStays)
            {
                REQUIRE(sheepSend * p.d >= wheatReceive * p.n);
            }
            else
            {
                REQUIRE(sheepSend * p.d <= wheatReceive * p.n);
            }
        };

        SECTION("price > 1")
        {
            // Exact boundary
            checkExchangeV10(Price{3, 2}, 3000, 4501, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 3000, 4500, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 3000, 4499, 2999, 4499);

            // Boundary between two values
            checkExchangeV10(Price{3, 2}, 2999, 4499, 2999, 4498);
            checkExchangeV10(Price{3, 2}, 2999, 4498, 2998, 4497);
        }

        SECTION("price < 1")
        {
            // Exact boundary
            checkExchangeV10(Price{2, 3}, 3000, 2001, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 3000, 2000, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 3000, 1999, 2998, 1999);

            // Boundary between two values
            checkExchangeV10(Price{2, 3}, 2999, 2000, 2999, 1999);
            checkExchangeV10(Price{2, 3}, 2999, 1999, 2998, 1999);
        }
    }

    SECTION("Limited by maxWheatReceive and maxSheepReceive")
    {
        auto checkExchangeV10 = [](Price const& p, int64_t maxWheatReceive,
                                   int64_t maxSheepReceive,
                                   int64_t wheatReceive, int64_t sheepSend) {
            auto res = exchangeV10(p, INT64_MAX, maxWheatReceive, INT64_MAX,
                                   maxSheepReceive, false);
            REQUIRE(res.wheatStays ==
                    (maxSheepReceive * p.d > maxWheatReceive * p.n));
            REQUIRE(res.numWheatReceived == wheatReceive);
            REQUIRE(res.numSheepSend == sheepSend);
            if (res.wheatStays)
            {
                REQUIRE(sheepSend * p.d >= wheatReceive * p.n);
            }
            else
            {
                REQUIRE(sheepSend * p.d <= wheatReceive * p.n);
            }
        };

        SECTION("price > 1")
        {
            // Exact boundary
            checkExchangeV10(Price{3, 2}, 3000, 4501, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 3000, 4500, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 3000, 4499, 2999, 4498);

            // Boundary between two values
            checkExchangeV10(Price{3, 2}, 2999, 4499, 2999, 4499);
            checkExchangeV10(Price{3, 2}, 2999, 4498, 2998, 4497);
        }

        SECTION("price < 1")
        {
            // Exact boundary
            checkExchangeV10(Price{2, 3}, 3000, 2001, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 3000, 2000, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 3000, 1999, 2999, 1999);

            // Boundary between two values
            checkExchangeV10(Price{2, 3}, 2999, 2000, 2998, 1999);
            checkExchangeV10(Price{2, 3}, 2999, 1999, 2999, 1999);
        }
    }

    SECTION("Limited by maxWheatSend and maxWheatReceive")
    {
        auto checkExchangeV10 = [](Price const& p, int64_t maxWheatSend,
                                   int64_t maxWheatReceive,
                                   int64_t wheatReceive, int64_t sheepSend) {
            auto res = exchangeV10(p, maxWheatSend, maxWheatReceive, INT64_MAX,
                                   INT64_MAX, false);
            REQUIRE(res.wheatStays == (maxWheatSend > maxWheatReceive));
            REQUIRE(res.numWheatReceived == wheatReceive);
            REQUIRE(res.numSheepSend == sheepSend);
            if (res.wheatStays)
            {
                REQUIRE(sheepSend * p.d >= wheatReceive * p.n);
            }
            else
            {
                REQUIRE(sheepSend * p.d <= wheatReceive * p.n);
            }
        };

        SECTION("price > 1")
        {
            // Exact boundary (boundary between values impossible in this case)
            checkExchangeV10(Price{3, 2}, 3000, 3001, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 3000, 3000, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 3000, 2999, 2999, 4499);
        }

        SECTION("price < 1")
        {
            // Exact boundary (boundary between values impossible in this case)
            checkExchangeV10(Price{2, 3}, 3000, 3001, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 3000, 3000, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 3000, 2999, 2998, 1999);
        }
    }

    SECTION("Limited by maxSheepSend and maxSheepReceive")
    {
        auto checkExchangeV10 = [](Price const& p, int64_t maxSheepSend,
                                   int64_t maxSheepReceive,
                                   int64_t wheatReceive, int64_t sheepSend) {
            auto res = exchangeV10(p, INT64_MAX, INT64_MAX, maxSheepSend,
                                   maxSheepReceive, false);
            REQUIRE(res.wheatStays == (maxSheepReceive > maxSheepSend));
            REQUIRE(res.numWheatReceived == wheatReceive);
            REQUIRE(res.numSheepSend == sheepSend);
            if (res.wheatStays)
            {
                REQUIRE(sheepSend * p.d >= wheatReceive * p.n);
            }
            else
            {
                REQUIRE(sheepSend * p.d <= wheatReceive * p.n);
            }
        };

        SECTION("price > 1")
        {
            // Exact boundary (boundary between values impossible in this case)
            checkExchangeV10(Price{3, 2}, 4500, 4501, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 4500, 4500, 3000, 4500);
            checkExchangeV10(Price{3, 2}, 4500, 4499, 2999, 4498);
        }

        SECTION("price < 1")
        {
            // Exact boundary (boundary between values impossible in this case)
            checkExchangeV10(Price{2, 3}, 2000, 2001, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 2000, 2000, 3000, 2000);
            checkExchangeV10(Price{2, 3}, 2000, 1999, 2999, 1999);
        }
    }

    SECTION("Threshold")
    {
        auto checkExchangeV10 = [](Price const& p, int64_t maxWheatSend,
                                   int64_t maxWheatReceive,
                                   int64_t wheatReceive, int64_t sheepSend) {
            auto res = exchangeV10(p, maxWheatSend, maxWheatReceive, INT64_MAX,
                                   INT64_MAX, false);
            REQUIRE(res.wheatStays == (maxWheatSend > maxWheatReceive));
            REQUIRE(res.numWheatReceived == wheatReceive);
            REQUIRE(res.numSheepSend == sheepSend);
            if (res.wheatStays)
            {
                REQUIRE(sheepSend * p.d >= wheatReceive * p.n);
            }
            else
            {
                REQUIRE(sheepSend * p.d <= wheatReceive * p.n);
            }
        };

        // Exchange nothing if thresholds exceeded
        checkExchangeV10(Price{3, 2}, 28, 27, 0, 0);
        checkExchangeV10(Price{3, 2}, 28, 26, 26, 39);

        // Thresholds not exceeded for sufficiently large offers
        checkExchangeV10(Price{3, 2}, 52, 51, 51, 77);
        checkExchangeV10(Price{3, 2}, 52, 50, 50, 75);
    }

    SECTION("isPathPayment")
    {
        auto check = [](Price const& p, int64_t maxWheatSend,
                        int64_t maxWheatReceive, bool isPathPayment,
                        int64_t wheatReceive, int64_t sheepSend) {
            auto res = exchangeV10(p, maxWheatSend, maxWheatReceive, INT64_MAX,
                                   INT64_MAX, isPathPayment);
            REQUIRE(res.wheatStays == (maxWheatSend > maxWheatReceive));
            REQUIRE(res.numWheatReceived == wheatReceive);
            REQUIRE(res.numSheepSend == sheepSend);
        };

        SECTION("no thresholding")
        {
            check(Price{3, 2}, 28, 27, false, 0, 0);
            check(Price{3, 2}, 28, 27, true, 27, 41);
        }

        SECTION("result is unchanged if wheat is more valuable")
        {
            check(Price{3, 2}, 150, 101, false, 101, 152);
            check(Price{3, 2}, 150, 101, true, 101, 152);
        }

        SECTION("transfer can increase if sheep is more valuable")
        {
            check(Price{2, 3}, 150, 101, false, 100, 67);
            check(Price{2, 3}, 150, 101, true, 101, 68);
        }
    }
}

TEST_CASE("Adjust Offer", "[exchange]")
{
    auto checkAdjustOffer = [](Price const& p, int64_t maxWheatSend,
                               int64_t maxSheepReceive,
                               int64_t expectedAmount) {
        int64_t adjAmount = adjustOffer(p, maxWheatSend, maxSheepReceive);
        REQUIRE(adjAmount == expectedAmount);
    };

    SECTION("Limits")
    {
        SECTION("price > 1")
        {
            SECTION("limited by maxWheatSend")
            {
                checkAdjustOffer(Price{1, 1000}, 2001, INT64_MAX, 2000);
                checkAdjustOffer(Price{1, 1000}, 2000, INT64_MAX, 2000);
                checkAdjustOffer(Price{1, 1000}, 1999, INT64_MAX, 1000);
            }

            SECTION("limited (or not) by maxSheepReceive")
            {
                checkAdjustOffer(Price{1, 1000}, 2000, 3, 2000);
                checkAdjustOffer(Price{1, 1000}, 2000, 2, 2000);
                checkAdjustOffer(Price{1, 1000}, 2000, 1, 1000);
            }
        }

        SECTION("price < 1")
        {
            SECTION("limited by maxWheatSend")
            {
                checkAdjustOffer(Price{1000, 1}, 401, INT64_MAX, 401);
                checkAdjustOffer(Price{1000, 1}, 400, INT64_MAX, 400);
                checkAdjustOffer(Price{1000, 1}, 399, INT64_MAX, 399);
            }

            SECTION("limited (or not) by maxSheepReceive")
            {
                checkAdjustOffer(Price{1000, 1}, 400, 400 * 1000 + 1, 400);
                checkAdjustOffer(Price{1000, 1}, 400, 400 * 1000, 400);
                checkAdjustOffer(Price{1000, 1}, 400, 400 * 1000 - 1, 399);
            }
        }
    }

    SECTION("Adjusting offer again has no effect")
    {
        auto checkAdjustOfferTwice = [&](Price const& p, int64_t maxWheatSend,
                                         int64_t maxSheepReceive,
                                         int64_t expectedAmount) {
            checkAdjustOffer(p, maxWheatSend, maxSheepReceive, expectedAmount);
            checkAdjustOffer(p, expectedAmount, maxSheepReceive,
                             expectedAmount);
        };

        SECTION("price > 1")
        {
            SECTION("limited by maxWheatSend")
            {
                checkAdjustOfferTwice(Price{7, 3}, 429, INT64_MAX, 429);
                checkAdjustOfferTwice(Price{7, 3}, 428, INT64_MAX, 428);
                checkAdjustOfferTwice(Price{7, 3}, 427, INT64_MAX, 427);
            }

            SECTION("limited (or not) by maxSheepReceive")
            {
                checkAdjustOfferTwice(Price{7, 3}, 428, 999, 428);
                checkAdjustOfferTwice(Price{7, 3}, 428, 998, 427);
                checkAdjustOfferTwice(Price{7, 3}, 428, 997, 427);
            }
        }

        SECTION("price < 1")
        {
            SECTION("limited by maxWheatSend")
            {
                checkAdjustOfferTwice(Price{3, 7}, 1001, INT64_MAX, 1001);
                checkAdjustOfferTwice(Price{3, 7}, 1000, INT64_MAX, 999);
                checkAdjustOfferTwice(Price{3, 7}, 999, INT64_MAX, 999);
            }

            SECTION("limited (or not) by maxSheepReceive")
            {
                checkAdjustOfferTwice(Price{3, 7}, 1000, 429, 999);
                checkAdjustOfferTwice(Price{3, 7}, 1000, 428, 999);
                checkAdjustOfferTwice(Price{3, 7}, 1000, 427, 997);
            }
        }
    }

    SECTION("Thresholds")
    {
        // Thresholds effect some small offers but not all
        checkAdjustOffer(Price{3, 2}, 29, INT64_MAX, 0);
        checkAdjustOffer(Price{3, 2}, 28, INT64_MAX, 28);
        checkAdjustOffer(Price{3, 2}, 27, INT64_MAX, 0);
        checkAdjustOffer(Price{3, 2}, 26, INT64_MAX, 26);

        // Thresholds don't effect sufficiently large offers
        checkAdjustOffer(Price{3, 2}, 51, INT64_MAX, 51);
        checkAdjustOffer(Price{3, 2}, 50, INT64_MAX, 50);
    }
}

TEST_CASE("Check price error bounds", "[exchange]")
{
    auto validateBounds = [](Price const& p, int64_t wheatReceive,
                             int64_t sheepSendHigh, int64_t sheepSendLow,
                             bool canFavorWheat) {
        REQUIRE(checkPriceErrorBound(p, wheatReceive, sheepSendHigh + 1,
                                     canFavorWheat) == canFavorWheat);
        REQUIRE(checkPriceErrorBound(p, wheatReceive, sheepSendHigh,
                                     canFavorWheat));
        REQUIRE(
            checkPriceErrorBound(p, wheatReceive, sheepSendLow, canFavorWheat));
        REQUIRE(!checkPriceErrorBound(p, wheatReceive, sheepSendLow - 1,
                                      canFavorWheat));
    };

    for (bool canFavorWheat : {false, true})
    {
        SECTION(canFavorWheat ? "can favor wheat" : "cannot favor wheat")
        {
            // No rounding
            validateBounds(Price{1, 1}, 1000, 1010, 990, canFavorWheat);
            // No rounding on boundary, p > 1
            validateBounds(Price{5, 2}, 1000, 2525, 2475, canFavorWheat);
            // No rounding on boundary, p < 1
            validateBounds(Price{2, 5}, 1000, 404, 396, canFavorWheat);
            // Rounding on boundary, p > 1
            validateBounds(Price{7, 3}, 1000, 2356, 2310, canFavorWheat);
            // Rounding on boundary, p > 1
            validateBounds(Price{3, 7}, 1000, 432, 425, canFavorWheat);
        }
    }
}
