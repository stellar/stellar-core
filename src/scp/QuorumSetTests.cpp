// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "scp/QuorumSetUtils.h"
#include "simulation/Simulation.h"

namespace stellar
{

TEST_CASE("sane quorum set", "[scp][quorumset]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    auto check = [&](SCPQuorumSet const& qSetCheck, bool expected,
                     SCPQuorumSet const& expectedSelfQSet)
    {
        // first, without normalization
        {
            SCPQuorumSet localSet;
            localSet.threshold = 1;
            SIMULATION_CREATE_NODE(100);
            localSet.validators.emplace_back(v100SecretKey.getPublicKey());

            REQUIRE(expected == isQuorumSetSane(qSetCheck, false));
        }
        // secondary test: attempts to build local node with the set
        // (this normalizes the set)

        auto normalizedQSet = qSetCheck;
        normalizeQSet(normalizedQSet);
        bool selfIsSane = isQuorumSetSane(normalizedQSet, false);

        REQUIRE(expected == selfIsSane);
        REQUIRE(expectedSelfQSet == normalizedQSet);
    };

    SCPQuorumSet qSet;

    qSet.threshold = 0;
    qSet.validators.push_back(v0NodeID);

    // { t: 0, v0 }
    check(qSet, false, qSet);

    qSet.threshold = 2;
    SCPQuorumSet qSetCheck2;
    qSetCheck2 = qSet;
    // { t: 2, v0 }
    check(qSet, false, qSetCheck2);

    qSet.threshold = 1;
    // { t: 1, v0 }
    check(qSet, true, qSet);

    // { t: 1, v0, { t: 1, v1 } }
    // -> { t:1, v0, v1 }
    SCPQuorumSet qSet2;
    qSet2 = qSet;
    {
        SCPQuorumSet qSetV1;
        qSetV1.threshold = 1;
        qSetV1.validators.push_back(v1NodeID);
        qSet2.innerSets.push_back(qSetV1);
    }

    SCPQuorumSet qSetV0V1;
    qSetV0V1 = qSet;
    qSetV0V1.validators.push_back(v1NodeID);
    check(qSet2, true, qSetV0V1);

    // { t: 1, v0, { t: 1, v1 }, { t: 2, v2 } }
    // -> { t:1, v0, v1, { t: 2, v2 } }
    {
        SCPQuorumSet qSet2bad;
        SCPQuorumSet qSet2t2;
        qSet2t2.threshold = 2;
        qSet2t2.validators.push_back(v2NodeID);

        qSet2bad = qSet2;
        qSet2bad.innerSets.push_back(qSet2t2);
        SCPQuorumSet qSetV0V1t2V2;
        qSetV0V1t2V2 = qSetV0V1;
        qSetV0V1t2V2.innerSets.push_back(qSet2t2);
        check(qSet2bad, false, qSetV0V1t2V2);
    }

    // qSet2 { t: 1, v0, { t: 1, v1 }, { t: 1, v2, v3 } }
    // -> qSet3 { t:1, v0, v1, { t: 1, v2, v3 } }
    SCPQuorumSet qSetV2V3;
    qSetV2V3.threshold = 1;
    qSetV2V3.validators.push_back(v2NodeID);
    qSetV2V3.validators.push_back(v3NodeID);
    qSet2.innerSets.push_back(qSetV2V3);
    SCPQuorumSet qSet3;
    qSet3 = qSetV0V1;
    qSet3.innerSets.push_back(qSetV2V3);
    check(qSet2, true, qSet3);

    // { t: 1, qSet2 }
    // --> qSet3
    SCPQuorumSet qSetWrapper;
    qSetWrapper.threshold = 1;
    qSetWrapper.innerSets.push_back(qSet2);
    check(qSet2, true, qSet3);
}

}
