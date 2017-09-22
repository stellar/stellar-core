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

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("bump sequence", "[tx][bumpsequence]")
{
    using xdr::operator==;

    Config const& cfg = getTestConfig();

    VirtualClock clock;
    ApplicationEditableVersion app(clock, cfg);
    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);
    auto a1 = root.create("A", app.getLedgerManager().getMinBalance(0) + 1000);
    auto a2 = root.create("B", app.getLedgerManager().getMinBalance(0) + 1000);
    std::vector<SecretKey> signers {a2.getSecretKey(),};


    SECTION("tests without a range")
    {
        auto num = a1.loadSequenceNumber();
        SECTION("test no-account")
        {

            auto not_created = TestAccount{app, getAccount("not created"), 1};
            std::vector<SecretKey> signers2 {not_created.getSecretKey()};
            for_all_versions(app, [&]{
                    REQUIRE_THROWS_AS(a1.bumpSequence(not_created.getPublicKey(), &signers2, num+110, nullptr),
                            ex_txNO_ACCOUNT);
                });

        }
        SECTION("test self-bump")
        {
            for_all_versions(app, [&]{
                    REQUIRE_THROWS_AS(a1.bumpSequence(a1.getPublicKey(),&signers, num+110, nullptr),
                            ex_BUMP_SEQ_NO_SELF_BUMP);
                    REQUIRE(num == a2.loadSequenceNumber());
                });
        }
        SECTION("test success")
        {
            for_all_versions(app, [&]{
                    auto num = a2.loadSequenceNumber();
                    REQUIRE_NOTHROW(a1.bumpSequence(a2.getPublicKey(),&signers, num+100, nullptr));
                    REQUIRE(num+100 == a2.loadSequenceNumber());
                });
        }
    }
    SECTION("tests with a range")
    {

        SECTION("test invalid-range")
        {
            BumpSeqValidRange invalid_range{1000, 100};
            for_all_versions(app, [&]{
                    auto num = a2.loadSequenceNumber();
                    REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, 110, &invalid_range),
                            ex_BUMP_SEQ_INVALID_RANGE);
                    REQUIRE(num == a2.loadSequenceNumber());
                });
        }
        SECTION("test out-of-range")
        {
            SECTION("below range")
            {
                for_all_versions(app, [&]{
                        auto num = a2.loadSequenceNumber();
                        BumpSeqValidRange below_range{num+10, num+20};
                        REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+300, &below_range),
                                ex_BUMP_SEQ_OUT_OF_RANGE);
                        REQUIRE(num == a2.loadSequenceNumber());
                    });
            }
            SECTION("above range")
            {
                for_all_versions(app, [&]{
                        auto num = a2.loadSequenceNumber();
                        BumpSeqValidRange above_range{num - 3, num-2};
                        REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+300, &above_range),
                                ex_BUMP_SEQ_OUT_OF_RANGE);
                        REQUIRE(num == a2.loadSequenceNumber());
                    });
            }
            SECTION("in range")
            {
                for_all_versions(app, [&]{
                        auto num = a2.loadSequenceNumber();
                        BumpSeqValidRange in_range{num - 10, num + 10};
                        REQUIRE_NOTHROW(a1.bumpSequence(a2.getPublicKey(),&signers, num + 100, &in_range));
                        REQUIRE(num+100 == a2.loadSequenceNumber());
                    });
            }
        }
        SECTION("test locked-range")
        {
            auto num = a2.loadSequenceNumber();
            BumpSeqValidRange exact_range{num, num};
            for_all_versions(app, [&]{
                    REQUIRE_NOTHROW(a1.bumpSequence(a2.getPublicKey(),&signers, num+100, &exact_range));
                    REQUIRE(num+100 == a2.loadSequenceNumber());
                });
        }
    }

}
