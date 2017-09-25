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

template <typename Callable>
void
for_versions_bumpseq(ApplicationEditableVersion& app, Callable&& f) {
    for_versions_to(std::max(8, //Config::CURRENT_LEDGER_PROTOCOL_VERSION // TODO: For some reason I'm getting a linker error on this const?
                9), app, f);
}

#define BUMPSEQ_ENABLED() (app.getLedgerManager().getCurrentLedgerVersion() >= 9)

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
        SECTION("test no-account")
        {

            SequenceNumber num = 1;
            auto not_created = TestAccount{app, getAccount("not created"), num};
            std::vector<SecretKey> signers2 {not_created.getSecretKey()};
            for_versions_bumpseq(app, [&]{
                    if (BUMPSEQ_ENABLED())
                        REQUIRE_THROWS_AS(a1.bumpSequence(not_created.getPublicKey(), &signers2, num+110, nullptr),
                                ex_txNO_ACCOUNT);
                    else
                        REQUIRE_THROWS_AS(a1.bumpSequence(not_created.getPublicKey(), &signers2, num+110, nullptr),
                                ex_txNO_ACCOUNT);
                });

        }
        SECTION("test self-bump")
        {
            for_versions_bumpseq(app, [&]{
                    auto num = a2.loadSequenceNumber();
                    if (BUMPSEQ_ENABLED())
                        REQUIRE_THROWS_AS(a1.bumpSequence(a1.getPublicKey(),&signers, num+110, nullptr),
                                ex_BUMP_SEQ_NO_SELF_BUMP);
                    else
                        REQUIRE_THROWS_AS(a1.bumpSequence(a1.getPublicKey(),&signers, num+110, nullptr),
                                ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                    REQUIRE(num == a2.loadSequenceNumber());
                });
        }
        SECTION("test success")
        {
            for_versions_bumpseq(app, [&]{
                    auto num = a2.loadSequenceNumber();
                    if (BUMPSEQ_ENABLED()) {
                        REQUIRE_NOTHROW(a1.bumpSequence(a2.getPublicKey(),&signers, num+100, nullptr));
                        REQUIRE(num+100 == a2.loadSequenceNumber());
                    } else {
                        REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+100, nullptr), ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                    }
                });
        }
    }
    SECTION("tests with a range")
    {

        SECTION("test invalid-range")
        {
            BumpSeqValidRange invalid_range{1000, 100};
            for_versions_bumpseq(app, [&]{
                    auto num = a2.loadSequenceNumber();
                    if (BUMPSEQ_ENABLED())
                        REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, 110, &invalid_range),
                                ex_BUMP_SEQ_INVALID_RANGE);
                    else
                        REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, 110, &invalid_range),
                                ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                    REQUIRE(num == a2.loadSequenceNumber());
                });
        }
        SECTION("test out-of-range")
        {
            SECTION("below range")
            {
                for_versions_bumpseq(app, [&]{
                        auto num = a2.loadSequenceNumber();
                        BumpSeqValidRange below_range{num+10, num+20};
                        if (BUMPSEQ_ENABLED())
                            REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+300, &below_range),
                                    ex_BUMP_SEQ_OUT_OF_RANGE);
                        else
                            REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+300, &below_range),
                                    ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                        REQUIRE(num == a2.loadSequenceNumber());
                    });
            }
            SECTION("above range")
            {
                for_versions_bumpseq(app, [&]{
                        auto num = a2.loadSequenceNumber();
                        BumpSeqValidRange above_range{num - 3, num-2};
                        if (BUMPSEQ_ENABLED())
                            REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+300, &above_range),
                                    ex_BUMP_SEQ_OUT_OF_RANGE);
                        else
                            REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+300, &above_range),
                                    ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                        REQUIRE(num == a2.loadSequenceNumber());
                    });
            }
            SECTION("in range")
            {
                for_versions_bumpseq(app, [&]{
                        auto num = a2.loadSequenceNumber();
                        BumpSeqValidRange in_range{num - 10, num + 10};
                        if (BUMPSEQ_ENABLED()) {
                            REQUIRE_NOTHROW(a1.bumpSequence(a2.getPublicKey(),&signers, num + 100, &in_range));
                            REQUIRE(num+100 == a2.loadSequenceNumber());
                        } else {
                            REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num + 100, &in_range), ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                        }
                    });
            }
        }
        SECTION("test locked-range")
        {
            auto num = a2.loadSequenceNumber();
            BumpSeqValidRange exact_range{num, num};
            for_versions_bumpseq(app, [&]{
                    if (BUMPSEQ_ENABLED()) {
                        REQUIRE_NOTHROW(a1.bumpSequence(a2.getPublicKey(),&signers, num+100, &exact_range));
                        REQUIRE(num+100 == a2.loadSequenceNumber());
                    } else {
                        REQUIRE_THROWS_AS(a1.bumpSequence(a2.getPublicKey(),&signers, num+100, &exact_range), ex_BUMP_SEQ_NOT_SUPPORTED_YET);
                    }
                });
        }
    }

}
