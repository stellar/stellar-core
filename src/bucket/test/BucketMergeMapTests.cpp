// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketMergeMap.h"
#include "bucket/BucketTests.h"
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
        std::vector<LedgerEntry> live(numEntries);
        for (auto& e : live)
        {
            e = LedgerTestUtils::generateValidLedgerEntry(3);
        }
        std::shared_ptr<Bucket> b1 =
            Bucket::fresh(app->getBucketManager(),
                          BucketTests::getAppLedgerVersion(app), {}, live, {},
                          /*countMergeEvents=*/true, clock.getIOContext(),
                          /*doFsync=*/true);
        return b1;
    };

    std::shared_ptr<Bucket> in1a = getValidBucket();
    std::shared_ptr<Bucket> in1b = getValidBucket();
    std::shared_ptr<Bucket> in1c = getValidBucket();

    std::shared_ptr<Bucket> in2a = getValidBucket();
    std::shared_ptr<Bucket> in2b = getValidBucket();
    std::shared_ptr<Bucket> in2c = getValidBucket();

    std::shared_ptr<Bucket> in3a = getValidBucket();
    std::shared_ptr<Bucket> in3b = getValidBucket();
    std::shared_ptr<Bucket> in3c = getValidBucket();
    std::shared_ptr<Bucket> in3d = getValidBucket();

    std::shared_ptr<Bucket> in4a = getValidBucket();
    std::shared_ptr<Bucket> in4b = getValidBucket();

    std::shared_ptr<Bucket> in5a = getValidBucket();
    std::shared_ptr<Bucket> in5b = getValidBucket();

    std::shared_ptr<Bucket> in6a = getValidBucket();
    std::shared_ptr<Bucket> in6b = getValidBucket();

    std::shared_ptr<Bucket> out1 = getValidBucket();
    std::shared_ptr<Bucket> out2 = getValidBucket();
    std::shared_ptr<Bucket> out4 = getValidBucket();
    std::shared_ptr<Bucket> out6 = getValidBucket();

    BucketMergeMap bmm;

    MergeKey m1{true, in1a, in1b, {in1c}};
    MergeKey m2{true, in2a, in2b, {in2c}};
    MergeKey m3{true, in3a, in3b, {in3c, in3d}};
    MergeKey m4{true, in4a, in4b, {}};
    MergeKey m5{true, in5a, in5b, {}};
    MergeKey m6{true, in6a, in6b, {in1a}};

    bmm.recordMerge(m1, out1->getHashID());
    bmm.recordMerge(m2, out2->getHashID());
    // m3 produces same as m2
    bmm.recordMerge(m3, out2->getHashID());
    bmm.recordMerge(m4, out4->getHashID());
    // m5 isn't recorded
    // m6 reuses an input from m1
    bmm.recordMerge(m6, out6->getHashID());

    HashID t;
    REQUIRE(bmm.findMergeFor(m1, t));
    REQUIRE(t == out1->getHashID());
    REQUIRE(bmm.findMergeFor(m2, t));
    REQUIRE(t == out2->getHashID());
    REQUIRE(bmm.findMergeFor(m3, t));
    REQUIRE(t == out2->getHashID());
    REQUIRE(bmm.findMergeFor(m4, t));
    REQUIRE(t == out4->getHashID());
    REQUIRE(!bmm.findMergeFor(m5, t));
    REQUIRE(bmm.findMergeFor(m6, t));
    REQUIRE(t == out6->getHashID());

    std::set<HashID> outs;
    bmm.getOutputsUsingInput(in1a->getHashID(), outs);
    REQUIRE(outs == std::set<HashID>{out1->getHashID(), out6->getHashID()});
    outs.clear();
    bmm.getOutputsUsingInput(in1b->getHashID(), outs);
    REQUIRE(outs == std::set<HashID>{out1->getHashID()});
    outs.clear();
    bmm.getOutputsUsingInput(in1c->getHashID(), outs);
    REQUIRE(outs == std::set<HashID>{out1->getHashID()});

    REQUIRE(bmm.forgetAllMergesProducing(out1->getHashID()) ==
            UnorderedSet<MergeKey>{m1});
    REQUIRE(!bmm.findMergeFor(m1, t));
    outs.clear();
    bmm.getOutputsUsingInput(in1a->getHashID(), outs);
    REQUIRE(outs == std::set<HashID>{out6->getHashID()});

    REQUIRE(bmm.forgetAllMergesProducing(out2->getHashID()) ==
            UnorderedSet<MergeKey>{m2, m3});
    REQUIRE(!bmm.findMergeFor(m2, t));
    REQUIRE(!bmm.findMergeFor(m3, t));

    REQUIRE(bmm.forgetAllMergesProducing(out4->getHashID()) ==
            UnorderedSet<MergeKey>{m4});
    REQUIRE(!bmm.findMergeFor(m4, t));

    REQUIRE(bmm.forgetAllMergesProducing(out6->getHashID()) ==
            UnorderedSet<MergeKey>{m6});
    REQUIRE(!bmm.findMergeFor(m6, t));
    outs.clear();
    bmm.getOutputsUsingInput(in6a->getHashID(), outs);
    REQUIRE(outs == std::set<HashID>{});
    outs.clear();
    bmm.getOutputsUsingInput(in1a->getHashID(), outs);
    REQUIRE(outs == std::set<HashID>{});

    // Second forget produces empty set.
    REQUIRE(bmm.forgetAllMergesProducing(out1->getHashID()) ==
            UnorderedSet<MergeKey>{});
}
