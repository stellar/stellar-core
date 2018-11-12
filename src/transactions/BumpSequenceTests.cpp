// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/XDROperators.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("bump sequence", "[tx][bumpsequence]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto& lm = app->getLedgerManager();

    auto a = root.create("A", lm.getLastMinBalance(0) + 1000);
    auto b = root.create("B", lm.getLastMinBalance(0) + 1000);

    SECTION("test success")
    {
        for_versions_from(10, *app, [&]() {
            SECTION("small bump")
            {
                auto newSeq = a.loadSequenceNumber() + 2;
                a.bumpSequence(newSeq);
                REQUIRE(a.loadSequenceNumber() == newSeq);
            }
            SECTION("large bump")
            {
                auto newSeq = INT64_MAX;
                a.bumpSequence(newSeq);
                REQUIRE(a.loadSequenceNumber() == newSeq);
                SECTION("no more tx when INT64_MAX is reached")
                {
                    REQUIRE_THROWS_AS(a.pay(root, 1), ex_txBAD_SEQ);
                }
            }
            SECTION("backward jump (no-op)")
            {
                auto oldSeq = a.loadSequenceNumber();
                a.bumpSequence(1);
                // tx consumes sequence, bumpSequence doesn't do anything
                REQUIRE(a.loadSequenceNumber() == oldSeq + 1);
            }
            SECTION("bad seq")
            {
                REQUIRE_THROWS_AS(a.bumpSequence(-1), ex_BUMP_SEQUENCE_BAD_SEQ);
                REQUIRE_THROWS_AS(a.bumpSequence(INT64_MIN),
                                  ex_BUMP_SEQUENCE_BAD_SEQ);
            }
        });
    }
    SECTION("not supported")
    {
        for_versions_to(9, *app, [&]() {
            REQUIRE_THROWS_AS(a.bumpSequence(1), ex_opNOT_SUPPORTED);
        });
    }
}
