// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"
#include "scp/QuorumSetUtils.h"
#include "xdr/Stellar-SCP.h"

namespace stellar
{

TEST_CASE("sane quorum set", "[scp][quorumset]")
{
    auto makePublicKey = [](int i) {
        auto hash = sha256("NODE_SEED_" + std::to_string(i));
        auto secretKey = SecretKey::fromSeed(hash);
        return secretKey.getPublicKey();
    };

    auto makeSingleton = [](const PublicKey& key) {
        auto result = SCPQuorumSet{};
        result.threshold = 1;
        result.validators.push_back(key);
        return result;
    };

    auto keys = std::vector<PublicKey>{};
    for (auto i = 0; i < 1001; i++)
    {
        keys.push_back(makePublicKey(i));
    }

    auto check = [&](SCPQuorumSet const& qSetCheck, bool expected,
                     SCPQuorumSet const& expectedSelfQSet) {
        // first, without normalization
        REQUIRE(expected == isQuorumSetSane(qSetCheck, false));

        // secondary test: attempts to build local node with the set
        // (this normalizes the set)
        auto normalizedQSet = qSetCheck;
        normalizeQSet(normalizedQSet);
        auto selfIsSane = isQuorumSetSane(qSetCheck, false);

        REQUIRE(expected == selfIsSane);
        REQUIRE(expectedSelfQSet == normalizedQSet);
    };

    SECTION("{ t: 0 }")
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = 0;
        check(qSet, false, qSet);
    }

    auto validOneNode = makeSingleton(keys[0]);

    SECTION("{ t: 0, v0 }")
    {
        auto qSet = validOneNode;
        qSet.threshold = 0;
        check(qSet, false, qSet);
    }

    SECTION("{ t: 2, v0 }")
    {
        auto qSet = validOneNode;
        qSet.threshold = 2;
        check(qSet, false, qSet);
    }

    SECTION("{ t: 1, v0 }")
    {
        check(validOneNode, true, validOneNode);
    }

    SECTION("{ t: 1, v0, { t: 1, v1 } -> { t:1, v0, v1 }")
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = 1;
        qSet.validators.push_back(keys[0]);

        auto qSelfSet = qSet;
        qSelfSet.validators.push_back(keys[1]);

        qSet.innerSets.push_back({});
        qSet.innerSets.back().threshold = 1;
        qSet.innerSets.back().validators.push_back(keys[1]);

        check(qSet, true, qSelfSet);
    }

    SECTION("{ t: 1, v0, { t: 1, v1 }, { t: 2, v2 } } -> { t:1, v0, v1, { t: "
            "2, v2 } }")
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = 1;
        qSet.validators.push_back(keys[0]);

        qSet.innerSets.push_back({});
        qSet.innerSets.back().threshold = 2;
        qSet.innerSets.back().validators.push_back(keys[1]);

        auto qSelfSet = qSet;
        qSelfSet.validators.push_back(keys[2]);

        qSet.innerSets.push_back({});
        qSet.innerSets.back().threshold = 1;
        qSet.innerSets.back().validators.push_back(keys[2]);

        check(qSet, false, qSelfSet);
    }

    auto validMultipleNodes = SCPQuorumSet{};
    validMultipleNodes.threshold = 1;
    validMultipleNodes.validators.push_back(keys[0]);
    validMultipleNodes.innerSets.push_back({});
    validMultipleNodes.innerSets.back().threshold = 1;
    validMultipleNodes.innerSets.back().validators.push_back(keys[1]);
    validMultipleNodes.innerSets.push_back({});
    validMultipleNodes.innerSets.back().threshold = 1;
    validMultipleNodes.innerSets.back().validators.push_back(keys[2]);
    validMultipleNodes.innerSets.back().validators.push_back(keys[3]);

    auto validMultipleNodesNormalized = SCPQuorumSet{};
    validMultipleNodesNormalized.threshold = 1;
    validMultipleNodesNormalized.validators.push_back(keys[0]);
    validMultipleNodesNormalized.validators.push_back(keys[1]);
    validMultipleNodesNormalized.innerSets.push_back({});
    validMultipleNodesNormalized.innerSets.back().threshold = 1;
    validMultipleNodesNormalized.innerSets.back().validators.push_back(keys[2]);
    validMultipleNodesNormalized.innerSets.back().validators.push_back(keys[3]);

    SECTION("{ t: 1, v0, { t: 1, v1 }, { t: 1, v2, v3 } } -> { t:1, v0, v1, { "
            "t: 1, v2, v3 } }")
    {
        check(validMultipleNodes, true, validMultipleNodesNormalized);
    }

    SECTION("{ t: 1, { t: 1, v0, { t: 1, v1 }, { t: 1, v2, v3 } } } -> { t:1, "
            "v0, v1, { t: 1, v2, v3 } }")
    {
        auto containingSet = SCPQuorumSet{};
        containingSet.threshold = 1;
        containingSet.innerSets.push_back(validMultipleNodes);

        check(containingSet, true, validMultipleNodesNormalized);
    }

    SECTION("{ t: 1, v0, { t: 1, v1, { t: 1, v2 } } } -> { t: 1, v0, { t: 1, "
            "v1, v2 } }")
    {
        auto qSet = makeSingleton(keys[0]);
        auto qSet1 = makeSingleton(keys[1]);
        auto qSet2 = makeSingleton(keys[2]);
        qSet1.innerSets.push_back(qSet2);
        qSet.innerSets.push_back(qSet1);

        auto qSelfSet = SCPQuorumSet{};
        qSelfSet.threshold = 1;
        qSelfSet.validators.push_back(keys[0]);
        qSelfSet.innerSets.push_back({});
        qSelfSet.innerSets.back().threshold = 1;
        qSelfSet.innerSets.back().validators.push_back(keys[1]);
        qSelfSet.innerSets.back().validators.push_back(keys[2]);

        check(qSet, true, qSelfSet);
    }

    SECTION(
        "{ t: 1, v0, { t: 1, v1, { t: 1, v2, { t: 1, v3 } } } } -> too deep")
    {
        auto qSet = makeSingleton(keys[0]);
        auto qSet1 = makeSingleton(keys[1]);
        auto qSet2 = makeSingleton(keys[2]);
        auto qSet3 = makeSingleton(keys[3]);
        qSet2.innerSets.push_back(qSet3);
        qSet1.innerSets.push_back(qSet2);
        qSet.innerSets.push_back(qSet1);

        auto qSelfSet = SCPQuorumSet{};
        qSelfSet.threshold = 1;
        qSelfSet.validators.push_back(keys[0]);
        qSelfSet.innerSets.push_back({});
        qSelfSet.innerSets.back().threshold = 1;
        qSelfSet.innerSets.back().validators.push_back(keys[1]);
        qSelfSet.innerSets.back().innerSets.push_back({});
        qSelfSet.innerSets.back().innerSets.back().threshold = 1;
        qSelfSet.innerSets.back().innerSets.back().validators.push_back(
            keys[2]);
        qSelfSet.innerSets.back().innerSets.back().validators.push_back(
            keys[3]);

        check(qSet, false, qSelfSet);
    }

    SECTION("{ t: 1, v0..v999 } -> { t: 1, v0..v999 }")
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = 1;
        for (auto i = 0; i < 1000; i++)
            qSet.validators.push_back(keys[i]);

        check(qSet, true, qSet);
    }

    SECTION("{ t: 1, v0..v1000 } -> too big")
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = 1;
        for (auto i = 0; i < 1001; i++)
            qSet.validators.push_back(keys[i]);

        check(qSet, false, qSet);
    }

    SECTION("{ t: 1, v0, { t: 1, v1..v100 }, { t: 1, v101..v200} ... { t: 1, "
            "v901..v1000} -> too big")
    {
        auto qSet = SCPQuorumSet{};
        qSet.threshold = 1;
        qSet.validators.push_back(keys[0]);
        for (auto i = 0; i < 10; i++)
        {
            qSet.innerSets.push_back({});
            qSet.innerSets.back().threshold = 1;
            for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
                qSet.innerSets.back().validators.push_back(keys[j]);
        }

        check(qSet, false, qSet);
    }
}
}
