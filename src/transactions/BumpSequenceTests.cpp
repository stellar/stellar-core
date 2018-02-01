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
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("bump sequence", "[tx][bumpsequence]")
{
    using xdr::operator==;

    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto& lm = app->getLedgerManager();

    auto a = root.create("A", lm.getMinBalance(0) + 1000);
    auto b = root.create("B", lm.getMinBalance(0) + 1000);

    // close the ledger (this is required as bumpseq cannot work for
    // an account just created)
    closeLedgerOn(*app, 2, 1, 1, 2018);

    SequenceNumber maxSeqNum =
        (uint64(lm.getCurrentLedgerHeader().ledgerSeq) << 32) - 1;

    for_versions_from(10, *app, [&]() {
        SECTION("test success")
        {
            SECTION("min jump")
            {
                auto newSeq = a.loadSequenceNumber() + 2;
                a.bumpSequence(newSeq);
                REQUIRE(a.loadSequenceNumber() == newSeq);
            }
            SECTION("max jump")
            {
                a.bumpSequence(maxSeqNum);
                REQUIRE(a.loadSequenceNumber() == maxSeqNum);
            }
        }
        SECTION("errors")
        {
            SECTION("too far")
            {
                auto prev = a.loadSequenceNumber();
                REQUIRE_THROWS_AS(a.bumpSequence(maxSeqNum + 1),
                                  ex_BUMP_SEQUENCE_TOO_FAR);
                REQUIRE(a.loadSequenceNumber() == prev + 1);
            }
        }
    });
    for_versions_to(9, *app, [&]() {
        REQUIRE_THROWS_AS(a.bumpSequence(maxSeqNum),
                          ex_BUMP_SEQUENCE_NOT_SUPPORTED_YET);
    });
}
