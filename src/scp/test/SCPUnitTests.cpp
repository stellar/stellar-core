#include "lib/catch.hpp"
#include "scp/LocalNode.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "simulation/Simulation.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{
bool
isNear(uint64 r, double target)
{
    double v = (double)r / (double)UINT64_MAX;
    return (std::abs(v - target) < .01);
}

TEST_CASE("nomination weight", "[scp]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);
    SIMULATION_CREATE_NODE(5);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    uint64 result = LocalNode::getNodeWeight(v2NodeID, qSet);

    REQUIRE(isNear(result, .75));

    result = LocalNode::getNodeWeight(v4NodeID, qSet);
    REQUIRE(result == 0);

    SCPQuorumSet iQSet;
    iQSet.threshold = 1;
    iQSet.validators.push_back(v4NodeID);
    iQSet.validators.push_back(v5NodeID);
    qSet.innerSets.push_back(iQSet);

    result = LocalNode::getNodeWeight(v4NodeID, qSet);

    REQUIRE(isNear(result, .6 * .5));
}

class TestNominationSCP : public SCPDriver
{
  public:
    SCP mSCP;
    TestNominationSCP(NodeID const& nodeID, SCPQuorumSet const& qSetLocal)
        : mSCP(*this, nodeID, true, qSetLocal)
    {
        auto localQSet =
            std::make_shared<SCPQuorumSet>(mSCP.getLocalQuorumSet());
        storeQuorumSet(localQSet);
    }

    void
    signEnvelope(SCPEnvelope&) override
    {
    }

    void
    storeQuorumSet(SCPQuorumSetPtr qSet)
    {
        Hash qSetHash = sha256(xdr::xdr_to_opaque(*qSet.get()));
        mQuorumSets[qSetHash] = qSet;
    }

    SCPDriver::ValidationLevel
    validateValue(uint64 slotIndex, Value const& value,
                  bool nomination) override
    {
        return SCPDriver::kFullyValidatedValue;
    }

    SCPQuorumSetPtr
    getQSet(Hash const& qSetHash) override
    {
        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {

            return mQuorumSets[qSetHash];
        }
        return SCPQuorumSetPtr();
    }

    void
    emitEnvelope(SCPEnvelope const& envelope) override
    {
    }

    ValueWrapperPtr
    combineCandidates(uint64 slotIndex,
                      ValueWrapperPtrSet const& candidates) override
    {
        return nullptr;
    }

    void
    setupTimer(uint64 slotIndex, int timerID, std::chrono::milliseconds timeout,
               std::function<void()> cb) override
    {
    }

    std::map<Hash, SCPQuorumSetPtr> mQuorumSets;

    Value const&
    getLatestCompositeCandidate(uint64 slotIndex)
    {
        static Value const emptyValue{};
        return emptyValue;
    }

    Hash
    getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const override
    {
        SHA256 hasher;
        for (auto const& v : vals)
        {
            hasher.add(v);
        }
        return hasher.finish();
    }
};

class NominationTestHandler : public NominationProtocol
{
  public:
    NominationTestHandler(Slot& s) : NominationProtocol(s)
    {
    }

    void
    setPreviousValue(Value const& v)
    {
        mPreviousValue = v;
    }

    void
    setRoundNumber(int32 n)
    {
        mRoundNumber = n;
    }

    void
    updateRoundLeaders()
    {
        NominationProtocol::updateRoundLeaders();
    }

    std::set<NodeID>&
    getRoundLeaders()
    {
        return mRoundLeaders;
    }

    uint64
    getNodePriority(NodeID const& nodeID, SCPQuorumSet const& qset)
    {
        return NominationProtocol::getNodePriority(nodeID, qset);
    }
};

static SCPQuorumSet
makeQSet(std::vector<NodeID> const& nodeIDs, int threshold, int total,
         int offset)
{
    SCPQuorumSet qSet;
    qSet.threshold = threshold;
    for (int i = 0; i < total; i++)
    {
        qSet.validators.push_back(nodeIDs[i + offset]);
    }
    return qSet;
}

// this test case display statistical information on the priority function used
// by nomination
TEST_CASE("nomination weight stats", "[scp][!hide]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);
    SIMULATION_CREATE_NODE(5);
    SIMULATION_CREATE_NODE(6);

    std::vector<NodeID> nodeIDs = {v0NodeID, v1NodeID, v2NodeID, v3NodeID,
                                   v4NodeID, v5NodeID, v6NodeID};

    int const totalSlots = 1000;
    int const maxRoundPerSlot = 5; // 5 -> 15 seconds
    int const totalRounds = totalSlots * maxRoundPerSlot;

    auto runTests = [&](SCPQuorumSet qSet) {
        std::map<NodeID, int> wins;

        TestNominationSCP nomSCP(v0NodeID, qSet);
        for (int s = 0; s < totalSlots; s++)
        {
            Slot slot(s, nomSCP.mSCP);

            NominationTestHandler nom(slot);

            Value v;
            v.emplace_back(uint8_t(s)); // anything will do as a value

            nom.setPreviousValue(v);

            for (int i = 0; i < maxRoundPerSlot; i++)
            {
                nom.setRoundNumber(i);
                nom.updateRoundLeaders();
                auto& l = nom.getRoundLeaders();
                REQUIRE(!l.empty());
                for (auto& w : l)
                {
                    wins[w]++;
                }
            }
        }
        return wins;
    };

    SECTION("flat quorum")
    {
        auto flatTest = [&](int threshold, int total) {
            auto qSet = makeQSet(nodeIDs, threshold, total, 0);

            auto wins = runTests(qSet);

            for (auto& w : wins)
            {
                double stats = double(w.second * 100) / double(totalRounds);
                CLOG_INFO(SCP, "Got {}{}", stats,
                          ((v0NodeID == w.first) ? " LOCAL" : ""));
            }
        };

        SECTION("3 out of 5")
        {
            flatTest(3, 5);
        }
        SECTION("2 out of 3")
        {
            flatTest(2, 3);
        }
    }
    SECTION("hierarchy")
    {
        auto qSet = makeQSet(nodeIDs, 3, 4, 0);

        auto qSetInner = makeQSet(nodeIDs, 2, 3, 4);
        qSet.innerSets.emplace_back(qSetInner);

        auto wins = runTests(qSet);

        for (auto& w : wins)
        {
            double stats = double(w.second * 100) / double(totalRounds);
            bool outer =
                std::any_of(qSet.validators.begin(), qSet.validators.end(),
                            [&](auto const& k) { return k == w.first; });
            CLOG_INFO(SCP, "Got {} {}", stats,
                      ((v0NodeID == w.first) ? "LOCAL"
                                             : (outer ? "OUTER" : "INNER")));
        }
    }
}

TEST_CASE("nomination two nodes win stats", "[scp][!hide]")
{
    int const nbRoundsForStats = 9;
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);
    SIMULATION_CREATE_NODE(5);
    SIMULATION_CREATE_NODE(6);

    std::vector<NodeID> nodeIDs = {v0NodeID, v1NodeID, v2NodeID, v3NodeID,
                                   v4NodeID, v5NodeID, v6NodeID};

    int const totalIter = 10000;

    // maxRounds is the number of rounds to evaluate in a row
    // the iteration is considered successful if validators could
    // agree on what to nominate before maxRounds is reached
    auto nominationLeaders = [&](int maxRounds, SCPQuorumSet qSetNode0,
                                 SCPQuorumSet qSetNode1) {
        TestNominationSCP nomSCP0(v0NodeID, qSetNode0);
        TestNominationSCP nomSCP1(v1NodeID, qSetNode1);

        int tot = 0;
        for (int g = 0; g < totalIter; g++)
        {
            Slot slot0(0, nomSCP0.mSCP);
            NominationTestHandler nom0(slot0);

            Slot slot1(0, nomSCP1.mSCP);
            NominationTestHandler nom1(slot1);

            Value v;
            v.emplace_back(uint8_t(g));
            nom0.setPreviousValue(v);
            nom1.setPreviousValue(v);

            bool res = true;

            bool v0Voted = false;
            bool v1Voted = false;

            int r = 0;
            do
            {
                nom0.setRoundNumber(r);
                nom1.setRoundNumber(r);
                nom0.updateRoundLeaders();
                nom1.updateRoundLeaders();

                auto& l0 = nom0.getRoundLeaders();
                REQUIRE(!l0.empty());
                auto& l1 = nom1.getRoundLeaders();
                REQUIRE(!l1.empty());

                auto updateVoted = [&](auto const& id, auto const& leaders,
                                       bool& voted) {
                    if (!voted)
                    {
                        voted = std::find(leaders.begin(), leaders.end(), id) !=
                                leaders.end();
                    }
                };

                // checks if id voted (any past round, including this one)
                // AND id is a leader this round
                auto findNode = [](auto const& id, bool idVoted,
                                   auto const& otherLeaders) {
                    bool r = (idVoted && std::find(otherLeaders.begin(),
                                                   otherLeaders.end(),
                                                   id) != otherLeaders.end());
                    return r;
                };

                updateVoted(v0NodeID, l0, v0Voted);
                updateVoted(v1NodeID, l1, v1Voted);

                // either both vote for v0 or both vote for v1
                res = findNode(v0NodeID, v0Voted, l1);
                res = res || findNode(v1NodeID, v1Voted, l0);
            } while (!res && ++r < maxRounds);

            tot += res ? 1 : 0;
        }
        return tot;
    };

    SECTION("flat quorum")
    {
        // test using the same quorum on all nodes
        auto flatTest = [&](int threshold, int total) {
            auto qSet = makeQSet(nodeIDs, threshold, total, 0);

            for (int maxRounds = 1; maxRounds <= nbRoundsForStats; maxRounds++)
            {
                int tot = nominationLeaders(maxRounds, qSet, qSet);
                double stats = double(tot * 100) / double(totalIter);
                CLOG_INFO(SCP, "Win rate for {} : {}", maxRounds, stats);
            }
        };

        SECTION("3 out of 5")
        {
            flatTest(3, 5);
        }
        SECTION("2 out of 3")
        {
            flatTest(2, 3);
        }
    }

    SECTION("hierarchy")
    {
        SECTION("same qSet")
        {
            auto qSet = makeQSet(nodeIDs, 3, 4, 0);

            auto qSetInner = makeQSet(nodeIDs, 2, 3, 4);
            qSet.innerSets.emplace_back(qSetInner);

            for (int maxRounds = 1; maxRounds <= nbRoundsForStats; maxRounds++)
            {
                int tot = nominationLeaders(maxRounds, qSet, qSet);
                double stats = double(tot * 100) / double(totalIter);
                CLOG_INFO(SCP, "Win rate for {} : {}", maxRounds, stats);
            }
        }
        SECTION("v0 is inner node for v1")
        {
            auto qSet0 = makeQSet(nodeIDs, 3, 4, 0);
            auto qSetInner0 = makeQSet(nodeIDs, 2, 3, 4);
            qSet0.innerSets.emplace_back(qSetInner0);

            // v1's qset: we move v0 into the inner set
            auto qSet1 = qSet0;
            REQUIRE(qSet1.validators[0] == v0NodeID);
            std::swap(qSet1.validators[0], qSet1.innerSets[0].validators[0]);

            for (int maxRounds = 1; maxRounds <= nbRoundsForStats; maxRounds++)
            {
                int tot = nominationLeaders(maxRounds, qSet0, qSet1);
                double stats = double(tot * 100) / double(totalIter);
                CLOG_INFO(SCP, "Win rate for {} : {}", maxRounds, stats);
            }
        }
    }
}
}
