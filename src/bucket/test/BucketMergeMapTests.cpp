// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketMergeMap.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"

using namespace stellar;

TEST_CASE("bucket merge map", "[bucket][bucketmergemap]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto getValidBucket = [&](int numEntries = 10) {
        std::vector<LedgerEntry> live =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                {CONFIG_SETTING}, numEntries);
        std::shared_ptr<LiveBucket> b1 = LiveBucket::fresh(
            app->getBucketManager(), BucketTestUtils::getAppLedgerVersion(app),
            {}, live, {},
            /*countMergeEvents=*/true, clock.getIOContext(),
            /*doFsync=*/true);
        return b1;
    };

    std::shared_ptr<LiveBucket> in1a = getValidBucket();
    std::shared_ptr<LiveBucket> in1b = getValidBucket();
    std::shared_ptr<LiveBucket> in1c = getValidBucket();

    std::shared_ptr<LiveBucket> in2a = getValidBucket();
    std::shared_ptr<LiveBucket> in2b = getValidBucket();
    std::shared_ptr<LiveBucket> in2c = getValidBucket();

    std::shared_ptr<LiveBucket> in3a = getValidBucket();
    std::shared_ptr<LiveBucket> in3b = getValidBucket();
    std::shared_ptr<LiveBucket> in3c = getValidBucket();
    std::shared_ptr<LiveBucket> in3d = getValidBucket();

    std::shared_ptr<LiveBucket> in4a = getValidBucket();
    std::shared_ptr<LiveBucket> in4b = getValidBucket();

    std::shared_ptr<LiveBucket> in5a = getValidBucket();
    std::shared_ptr<LiveBucket> in5b = getValidBucket();

    std::shared_ptr<LiveBucket> in6a = getValidBucket();
    std::shared_ptr<LiveBucket> in6b = getValidBucket();

    std::shared_ptr<LiveBucket> out1 = getValidBucket();
    std::shared_ptr<LiveBucket> out2 = getValidBucket();
    std::shared_ptr<LiveBucket> out4 = getValidBucket();
    std::shared_ptr<LiveBucket> out6 = getValidBucket();

    BucketMergeMap bmm;

    MergeKey m1{true, in1a->getHash(), in1b->getHash(), {in1c->getHash()}};
    MergeKey m2{true, in2a->getHash(), in2b->getHash(), {in2c->getHash()}};
    MergeKey m3{true,
                in3a->getHash(),
                in3b->getHash(),
                {in3c->getHash(), in3d->getHash()}};
    MergeKey m4{true, in4a->getHash(), in4b->getHash(), {}};
    MergeKey m5{true, in5a->getHash(), in5b->getHash(), {}};
    MergeKey m6{true, in6a->getHash(), in6b->getHash(), {in1a->getHash()}};

    bmm.recordMerge(m1, out1->getHash());
    bmm.recordMerge(m2, out2->getHash());
    // m3 produces same as m2
    bmm.recordMerge(m3, out2->getHash());
    bmm.recordMerge(m4, out4->getHash());
    // m5 isn't recorded
    // m6 reuses an input from m1
    bmm.recordMerge(m6, out6->getHash());

    Hash t;
    REQUIRE(bmm.findMergeFor(m1, t));
    REQUIRE(t == out1->getHash());
    REQUIRE(bmm.findMergeFor(m2, t));
    REQUIRE(t == out2->getHash());
    REQUIRE(bmm.findMergeFor(m3, t));
    REQUIRE(t == out2->getHash());
    REQUIRE(bmm.findMergeFor(m4, t));
    REQUIRE(t == out4->getHash());
    REQUIRE(!bmm.findMergeFor(m5, t));
    REQUIRE(bmm.findMergeFor(m6, t));
    REQUIRE(t == out6->getHash());

    std::set<Hash> outs;
    bmm.getOutputsUsingInput(in1a->getHash(), outs);
    REQUIRE(outs == std::set<Hash>{out1->getHash(), out6->getHash()});
    outs.clear();
    bmm.getOutputsUsingInput(in1b->getHash(), outs);
    REQUIRE(outs == std::set<Hash>{out1->getHash()});
    outs.clear();
    bmm.getOutputsUsingInput(in1c->getHash(), outs);
    REQUIRE(outs == std::set<Hash>{out1->getHash()});

    REQUIRE(bmm.forgetAllMergesProducing(out1->getHash()) ==
            UnorderedSet<MergeKey>{m1});
    REQUIRE(!bmm.findMergeFor(m1, t));
    outs.clear();
    bmm.getOutputsUsingInput(in1a->getHash(), outs);
    REQUIRE(outs == std::set<Hash>{out6->getHash()});

    REQUIRE(bmm.forgetAllMergesProducing(out2->getHash()) ==
            UnorderedSet<MergeKey>{m2, m3});
    REQUIRE(!bmm.findMergeFor(m2, t));
    REQUIRE(!bmm.findMergeFor(m3, t));

    REQUIRE(bmm.forgetAllMergesProducing(out4->getHash()) ==
            UnorderedSet<MergeKey>{m4});
    REQUIRE(!bmm.findMergeFor(m4, t));

    REQUIRE(bmm.forgetAllMergesProducing(out6->getHash()) ==
            UnorderedSet<MergeKey>{m6});
    REQUIRE(!bmm.findMergeFor(m6, t));
    outs.clear();
    bmm.getOutputsUsingInput(in6a->getHash(), outs);
    REQUIRE(outs == std::set<Hash>{});
    outs.clear();
    bmm.getOutputsUsingInput(in1a->getHash(), outs);
    REQUIRE(outs == std::set<Hash>{});

    // Second forget produces empty set.
    REQUIRE(bmm.forgetAllMergesProducing(out1->getHash()) ==
            UnorderedSet<MergeKey>{});
}
