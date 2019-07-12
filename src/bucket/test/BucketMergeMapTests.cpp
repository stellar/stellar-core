// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketMergeMap.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"

using namespace stellar;

TEST_CASE("bucket merge map", "[bucket][bucketmergemap]")
{
    Hash in1a = HashUtils::random();
    Hash in1b = HashUtils::random();
    Hash in1c = HashUtils::random();

    Hash in2a = HashUtils::random();
    Hash in2b = HashUtils::random();
    Hash in2c = HashUtils::random();

    Hash in3a = HashUtils::random();
    Hash in3b = HashUtils::random();
    Hash in3c = HashUtils::random();
    Hash in3d = HashUtils::random();

    Hash in4a = HashUtils::random();
    Hash in4b = HashUtils::random();

    Hash in5a = HashUtils::random();
    Hash in5b = HashUtils::random();

    Hash in6a = HashUtils::random();
    Hash in6b = HashUtils::random();

    Hash out1 = HashUtils::random();
    Hash out2 = HashUtils::random();
    Hash out4 = HashUtils::random();
    Hash out6 = HashUtils::random();

    BucketMergeMap bmm;

    MergeKey m1{1, true, in1a, in1b, {in1c}};
    MergeKey m2{1, true, in2a, in2b, {in2c}};
    MergeKey m3{1, true, in3a, in3b, {in3c, in3d}};
    MergeKey m4{1, true, in4a, in4b, {}};
    MergeKey m5{1, true, in5a, in5b, {}};
    MergeKey m6{1, true, in6a, in6b, {in1a}};

    bmm.recordMerge(m1, out1);
    bmm.recordMerge(m2, out2);
    // m3 produces same as m2
    bmm.recordMerge(m3, out2);
    bmm.recordMerge(m4, out4);
    // m5 isn't recorded
    // m6 reuses an input from m1
    bmm.recordMerge(m6, out6);

    Hash t;
    REQUIRE(bmm.findMergeFor(m1, t));
    REQUIRE(t == out1);
    REQUIRE(bmm.findMergeFor(m2, t));
    REQUIRE(t == out2);
    REQUIRE(bmm.findMergeFor(m3, t));
    REQUIRE(t == out2);
    REQUIRE(bmm.findMergeFor(m4, t));
    REQUIRE(t == out4);
    REQUIRE(!bmm.findMergeFor(m5, t));
    REQUIRE(bmm.findMergeFor(m6, t));
    REQUIRE(t == out6);

    std::set<Hash> outs;
    bmm.getOutputsUsingInput(in1a, outs);
    REQUIRE(outs == std::set<Hash>{out1, out6});
    outs.clear();
    bmm.getOutputsUsingInput(in1b, outs);
    REQUIRE(outs == std::set<Hash>{out1});
    outs.clear();
    bmm.getOutputsUsingInput(in1c, outs);
    REQUIRE(outs == std::set<Hash>{out1});

    REQUIRE(bmm.forgetAllMergesProducing(out1) ==
            std::unordered_set<MergeKey>{m1});
    REQUIRE(!bmm.findMergeFor(m1, t));
    outs.clear();
    bmm.getOutputsUsingInput(in1a, outs);
    REQUIRE(outs == std::set<Hash>{out6});

    REQUIRE(bmm.forgetAllMergesProducing(out2) ==
            std::unordered_set<MergeKey>{m2, m3});
    REQUIRE(!bmm.findMergeFor(m2, t));
    REQUIRE(!bmm.findMergeFor(m3, t));

    REQUIRE(bmm.forgetAllMergesProducing(out4) ==
            std::unordered_set<MergeKey>{m4});
    REQUIRE(!bmm.findMergeFor(m4, t));

    REQUIRE(bmm.forgetAllMergesProducing(out6) ==
            std::unordered_set<MergeKey>{m6});
    REQUIRE(!bmm.findMergeFor(m6, t));
    outs.clear();
    bmm.getOutputsUsingInput(in6a, outs);
    REQUIRE(outs == std::set<Hash>{});
    outs.clear();
    bmm.getOutputsUsingInput(in1a, outs);
    REQUIRE(outs == std::set<Hash>{});

    // Second forget produces empty set.
    REQUIRE(bmm.forgetAllMergesProducing(out1) ==
            std::unordered_set<MergeKey>{});
}
