// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "lib/catch.hpp"
#include "scp/LocalNode.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "simulation/Simulation.h"
#include "util/Logging.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

namespace stellar
{

using xdr::operator<;
using xdr::operator==;

#define CREATE_VALUE(X) \
    static const Hash X##ValueHash = sha256("SEED_VALUE_HASH_" #X); \
    static const Value X##Value = xdr::xdr_to_opaque(X##ValueHash);

CREATE_VALUE(x);
CREATE_VALUE(y);
CREATE_VALUE(z);

class TestSCP : public SCPDriver
{
  public:
    SCP mSCP;

    TestSCP(SecretKey const& secretKey, SCPQuorumSet const& qSetLocal,
            bool isValidator = true)
        : mSCP(*this, secretKey, isValidator, qSetLocal)
    {
        mPriorityLookup = [&](NodeID const& n) {
            return (n == secretKey.getPublicKey()) ? 1000 : 1;
        };

        mHashValueCalculator = [&](Value const& v) { return 0; };

        auto localQSet =
            std::make_shared<SCPQuorumSet>(mSCP.getLocalQuorumSet());
        storeQuorumSet(localQSet);
    }

    void
    signEnvelope(SCPEnvelope&) override
    {
    }

    bool
    verifyEnvelope(SCPEnvelope const& envelope) override
    {
        return true;
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

    void
    ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot) override
    {
        mHeardFromQuorums[slotIndex].push_back(ballot);
    }

    void
    valueExternalized(uint64 slotIndex, Value const& value) override
    {
        if (mExternalizedValues.find(slotIndex) != mExternalizedValues.end())
        {
            throw std::out_of_range("Value already externalized");
        }
        mExternalizedValues[slotIndex] = value;
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
        mEnvs.push_back(envelope);
    }

    // used to test BallotProtocol and bypass nomination
    bool
    bumpState(uint64 slotIndex, Value const& v)
    {
        return mSCP.getSlot(slotIndex, true)->bumpState(v, true);
    }

    bool
    nominate(uint64 slotIndex, Value const& value, bool timedout)
    {
        return mSCP.getSlot(slotIndex, true)->nominate(value, value, timedout);
    }

    // only used by nomination protocol
    Value
    combineCandidates(uint64 slotIndex,
                      std::set<Value> const& candidates) override
    {
        REQUIRE(candidates == mExpectedCandidates);
        REQUIRE(!mCompositeValue.empty());

        return mCompositeValue;
    }

    std::set<Value> mExpectedCandidates;
    Value mCompositeValue;

    // override the internal hashing scheme in order to make tests
    // more predictable.
    uint64
    computeHashNode(uint64 slotIndex, Value const& prev, bool isPriority,
                    int32_t roundNumber, NodeID const& nodeID) override
    {
        uint64 res;
        if (isPriority)
        {
            res = mPriorityLookup(nodeID);
        }
        else
        {
            res = 0;
        }
        return res;
    }

    // override the value hashing, to make tests more predictable.
    uint64
    computeValueHash(uint64 slotIndex, Value const& prev, int32_t roundNumber,
                     Value const& value) override
    {
        return mHashValueCalculator(value);
    }

    void
    setupTimer(uint64 slotIndex, int timerID, std::chrono::milliseconds timeout,
               std::function<void()> cb) override
    {
    }

    std::function<uint64(NodeID const&)> mPriorityLookup;
    std::function<uint64(Value const&)> mHashValueCalculator;

    std::map<Hash, SCPQuorumSetPtr> mQuorumSets;
    std::vector<SCPEnvelope> mEnvs;
    std::map<uint64, Value> mExternalizedValues;
    std::map<uint64, std::vector<SCPBallot>> mHeardFromQuorums;

    Value const&
    getLatestCompositeCandidate(uint64 slotIndex)
    {
        return mSCP.getSlot(slotIndex, true)->getLatestCompositeCandidate();
    }

    void
    receiveEnvelope(SCPEnvelope const& envelope)
    {
        mSCP.receiveEnvelope(envelope);
    }

    Slot&
    getSlot(uint64 index)
    {
        return *mSCP.getSlot(index, false);
    }

    std::vector<SCPEnvelope>
    getEntireState(uint64 index)
    {
        auto v = mSCP.getSlot(index, false)->getEntireCurrentState();
        return v;
    }

    SCPEnvelope
    getCurrentEnvelope(uint64 index, NodeID const& id)
    {
        auto r = getEntireState(index);
        auto it = std::find_if(r.begin(), r.end(), [&](SCPEnvelope const& e) {
            return e.statement.nodeID == id;
        });
        if (it != r.end())
        {
            return *it;
        }
        throw std::runtime_error("not found");
    }
};

static SCPEnvelope
makeEnvelope(SecretKey const& secretKey, uint64 slotIndex,
             SCPStatement const& statement)
{
    SCPEnvelope envelope;
    envelope.statement = statement;
    envelope.statement.nodeID = secretKey.getPublicKey();
    envelope.statement.slotIndex = slotIndex;

    envelope.signature = secretKey.sign(xdr::xdr_to_opaque(envelope.statement));

    return envelope;
}

static SCPEnvelope
makeExternalize(SecretKey const& secretKey, Hash const& qSetHash,
                uint64 slotIndex, SCPBallot const& commitBallot, uint32 nH)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_EXTERNALIZE);
    auto& ext = st.pledges.externalize();
    ext.commit = commitBallot;
    ext.nH = nH;
    ext.commitQuorumSetHash = qSetHash;

    return makeEnvelope(secretKey, slotIndex, st);
}

static SCPEnvelope
makeConfirm(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
            uint32 prepareCounter, SCPBallot const& b, uint32 nC, uint32 nH)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_CONFIRM);
    auto& con = st.pledges.confirm();
    con.ballot = b;
    con.nPrepared = prepareCounter;
    con.nCommit = nC;
    con.nH = nH;
    con.quorumSetHash = qSetHash;

    return makeEnvelope(secretKey, slotIndex, st);
}

static SCPEnvelope
makePrepare(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
            SCPBallot const& ballot, SCPBallot* prepared = nullptr,
            uint32 nC = 0, uint32 nH = 0, SCPBallot* preparedPrime = nullptr)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_PREPARE);
    auto& p = st.pledges.prepare();
    p.ballot = ballot;
    p.quorumSetHash = qSetHash;
    if (prepared)
    {
        p.prepared.activate() = *prepared;
    }

    p.nC = nC;
    p.nH = nH;

    if (preparedPrime)
    {
        p.preparedPrime.activate() = *preparedPrime;
    }

    return makeEnvelope(secretKey, slotIndex, st);
}

static SCPEnvelope
makeNominate(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
             std::vector<Value> votes, std::vector<Value> accepted)
{
    std::sort(votes.begin(), votes.end());
    std::sort(accepted.begin(), accepted.end());

    SCPStatement st;
    st.pledges.type(SCP_ST_NOMINATE);
    auto& nom = st.pledges.nominate();
    nom.quorumSetHash = qSetHash;
    for (auto const& v : votes)
    {
        nom.votes.emplace_back(v);
    }
    for (auto const& a : accepted)
    {
        nom.accepted.emplace_back(a);
    }
    return makeEnvelope(secretKey, slotIndex, st);
}

void
verifyPrepare(SCPEnvelope const& actual, SecretKey const& secretKey,
              Hash const& qSetHash, uint64 slotIndex, SCPBallot const& ballot,
              SCPBallot* prepared = nullptr, uint32 nC = 0, uint32 nH = 0,
              SCPBallot* preparedPrime = nullptr)
{
    auto exp = makePrepare(secretKey, qSetHash, slotIndex, ballot, prepared, nC,
                           nH, preparedPrime);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyConfirm(SCPEnvelope const& actual, SecretKey const& secretKey,
              Hash const& qSetHash, uint64 slotIndex, uint32 nPrepared,
              SCPBallot const& b, uint32 nC, uint32 nH)
{
    auto exp =
        makeConfirm(secretKey, qSetHash, slotIndex, nPrepared, b, nC, nH);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyExternalize(SCPEnvelope const& actual, SecretKey const& secretKey,
                  Hash const& qSetHash, uint64 slotIndex,
                  SCPBallot const& commit, uint32 nH)
{
    auto exp = makeExternalize(secretKey, qSetHash, slotIndex, commit, nH);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyNominate(SCPEnvelope const& actual, SecretKey const& secretKey,
               Hash const& qSetHash, uint64 slotIndex, std::vector<Value> votes,
               std::vector<Value> accepted)
{
    auto exp = makeNominate(secretKey, qSetHash, slotIndex, votes, accepted);
    REQUIRE(exp.statement == actual.statement);
}

TEST_CASE("vblocking and quorum", "[scp]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    std::vector<NodeID> nodeSet;
    nodeSet.push_back(v0NodeID);

    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == false);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == false);

    nodeSet.push_back(v2NodeID);

    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == false);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);

    nodeSet.push_back(v3NodeID);
    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == true);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);

    nodeSet.push_back(v1NodeID);
    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == true);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);
}

TEST_CASE("v-blocking distance", "[scp]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);
    SIMULATION_CREATE_NODE(5);
    SIMULATION_CREATE_NODE(6);
    SIMULATION_CREATE_NODE(7);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    auto check = [&](SCPQuorumSet const& qSetCheck, std::set<NodeID> const& s,
                     int expected) {
        auto r = LocalNode::findClosestVBlocking(qSetCheck, s, nullptr);
        REQUIRE(expected == r.size());
    };

    std::set<NodeID> good;
    good.insert(v0NodeID);

    // already v-blocking
    check(qSet, good, 0);

    good.insert(v1NodeID);
    // either v0 or v1
    check(qSet, good, 1);

    good.insert(v2NodeID);
    // any 2 of v0..v2
    check(qSet, good, 2);

    SCPQuorumSet qSubSet1;
    qSubSet1.threshold = 1;
    qSubSet1.validators.push_back(v3NodeID);
    qSubSet1.validators.push_back(v4NodeID);
    qSubSet1.validators.push_back(v5NodeID);
    qSet.innerSets.push_back(qSubSet1);

    good.insert(v3NodeID);
    // any 3 of v0..v3
    check(qSet, good, 3);

    good.insert(v4NodeID);
    // v0..v2
    check(qSet, good, 3);

    qSet.threshold = 1;
    // v0..v4
    check(qSet, good, 5);

    good.insert(v5NodeID);
    // v0..v5
    check(qSet, good, 6);

    SCPQuorumSet qSubSet2;
    qSubSet2.threshold = 2;
    qSubSet2.validators.push_back(v6NodeID);
    qSubSet2.validators.push_back(v7NodeID);

    qSet.innerSets.push_back(qSubSet2);
    // v0..v5
    check(qSet, good, 6);

    good.insert(v6NodeID);
    // v0..v5
    check(qSet, good, 6);

    good.insert(v7NodeID);
    // v0..v5 and one of 6,7
    check(qSet, good, 7);

    qSet.threshold = 4;
    // v6, v7
    check(qSet, good, 2);

    qSet.threshold = 3;
    // v0..v2
    check(qSet, good, 3);

    qSet.threshold = 2;
    // v0..v2 and one of v6,v7
    check(qSet, good, 4);
}

typedef std::function<SCPEnvelope(SecretKey const& sk)> genEnvelope;

using namespace std::placeholders;

static genEnvelope
makePrepareGen(Hash const& qSetHash, SCPBallot const& ballot,
               SCPBallot* prepared = nullptr, uint32 nC = 0, uint32 nH = 0,
               SCPBallot* preparedPrime = nullptr)
{
    return std::bind(makePrepare, _1, std::cref(qSetHash), 0, std::cref(ballot),
                     prepared, nC, nH, preparedPrime);
}

static genEnvelope
makeConfirmGen(Hash const& qSetHash, uint32 prepareCounter, SCPBallot const& b,
               uint32 nC, uint32 nH)
{
    return std::bind(makeConfirm, _1, std::cref(qSetHash), 0, prepareCounter,
                     std::cref(b), nC, nH);
}

static genEnvelope
makeExternalizeGen(Hash const& qSetHash, SCPBallot const& commitBallot,
                   uint32 nH)
{
    return std::bind(makeExternalize, _1, std::cref(qSetHash), 0,
                     std::cref(commitBallot), nH);
}

TEST_CASE("ballot protocol core5", "[scp][ballotprotocol]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);

    // we need 5 nodes to avoid sharing various thresholds:
    // v-blocking set size: 2
    // threshold: 4 = 3 + self or 4 others
    SCPQuorumSet qSet;
    qSet.threshold = 4;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);
    qSet.validators.push_back(v4NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey, qSet);

    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
    uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();

    REQUIRE(xValue < yValue);

    CLOG(INFO, "SCP") << "";
    CLOG(INFO, "SCP") << "BEGIN TEST";

    auto recvVBlockingChecks = [&](genEnvelope gen, bool withChecks) {
        SCPEnvelope e1 = gen(v1SecretKey);
        SCPEnvelope e2 = gen(v2SecretKey);

        // nothing should happen with first message
        size_t i = scp.mEnvs.size();
        scp.receiveEnvelope(e1);
        if (withChecks)
        {
            REQUIRE(scp.mEnvs.size() == i);
        }
        i++;
        scp.receiveEnvelope(e2);
        if (withChecks)
        {
            REQUIRE(scp.mEnvs.size() == i);
        }
    };

    auto recvVBlocking = std::bind(recvVBlockingChecks, _1, true);

    auto recvQuorumChecks = [&](genEnvelope gen, bool withChecks,
                                bool delayedQuorum) {
        SCPEnvelope e1 = gen(v1SecretKey);
        SCPEnvelope e2 = gen(v2SecretKey);
        SCPEnvelope e3 = gen(v3SecretKey);
        SCPEnvelope e4 = gen(v4SecretKey);

        scp.receiveEnvelope(e1);
        scp.receiveEnvelope(e2);
        size_t i = scp.mEnvs.size() + 1;
        scp.receiveEnvelope(e3);
        if (withChecks && !delayedQuorum)
        {
            REQUIRE(scp.mEnvs.size() == i);
        }
        // nothing happens with an extra vote (unless we're in delayedQuorum)
        scp.receiveEnvelope(e4);
        if (withChecks && delayedQuorum)
        {
            REQUIRE(scp.mEnvs.size() == i);
        }
    };
    auto recvQuorum = std::bind(recvQuorumChecks, _1, true, false);

    auto nodesAllPledgeToCommit = [&]() {
        SCPBallot b(1, xValue);
        SCPEnvelope prepare1 = makePrepare(v1SecretKey, qSetHash, 0, b);
        SCPEnvelope prepare2 = makePrepare(v2SecretKey, qSetHash, 0, b);
        SCPEnvelope prepare3 = makePrepare(v3SecretKey, qSetHash, 0, b);
        SCPEnvelope prepare4 = makePrepare(v4SecretKey, qSetHash, 0, b);

        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, b);

        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 2);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0][0] == b);

        // We have a quorum including us

        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, b, &b);

        scp.receiveEnvelope(prepare4);
        REQUIRE(scp.mEnvs.size() == 2);

        SCPEnvelope prepared1 = makePrepare(v1SecretKey, qSetHash, 0, b, &b);
        SCPEnvelope prepared2 = makePrepare(v2SecretKey, qSetHash, 0, b, &b);
        SCPEnvelope prepared3 = makePrepare(v3SecretKey, qSetHash, 0, b, &b);
        SCPEnvelope prepared4 = makePrepare(v4SecretKey, qSetHash, 0, b, &b);

        scp.receiveEnvelope(prepared4);
        scp.receiveEnvelope(prepared3);
        REQUIRE(scp.mEnvs.size() == 2);

        scp.receiveEnvelope(prepared2);
        REQUIRE(scp.mEnvs.size() == 3);

        // confirms prepared
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, b, &b, b.counter,
                      b.counter);

        // extra statement doesn't do anything
        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 3);
    };

    SECTION("bumpState x")
    {
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPBallot expectedBallot(1, xValue);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, expectedBallot);
    }

    SECTION("start <1,x>")
    {
        Value const& aValue = xValue;
        Value const& bValue = yValue;

        SCPBallot A1(1, aValue);
        SCPBallot B1(1, bValue);

        SCPBallot A2 = A1;
        A2.counter++;

        SCPBallot A3 = A2;
        A3.counter++;

        SCPBallot A4 = A3;
        A4.counter++;

        SCPBallot A5 = A4;
        A5.counter++;

        SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

        SCPBallot B2 = B1;
        B2.counter++;

        SCPBallot B3 = B2;
        B3.counter++;

        REQUIRE(scp.bumpState(0, aValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SECTION("prepared A1")
        {
            recvQuorum(makePrepareGen(qSetHash, A1));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &A1);

            SECTION("bump prepared A2")
            {
                // bump to (2,a)

                REQUIRE(scp.bumpState(0, aValue));
                REQUIRE(scp.mEnvs.size() == 3);
                verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A2, &A1);

                recvQuorum(makePrepareGen(qSetHash, A2));
                REQUIRE(scp.mEnvs.size() == 4);
                verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A2, &A2);

                SECTION("Confirm prepared A2")
                {
                    recvQuorum(makePrepareGen(qSetHash, A2, &A2));
                    REQUIRE(scp.mEnvs.size() == 5);
                    verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, A2,
                                  &A2, 2, 2);

                    SECTION("Accept commit")
                    {
                        SECTION("Quorum A2")
                        {
                            recvQuorum(makePrepareGen(qSetHash, A2, &A2, 2, 2));
                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyConfirm(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, 2, A2, 2, 2);

                            SECTION("Quorum prepared A3")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, A3, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 2, A3, 2, 2);

                                recvQuorum(
                                    makePrepareGen(qSetHash, A3, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 8);
                                verifyConfirm(scp.mEnvs[7], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);

                                SECTION("Accept more commit A3")
                                {
                                    recvQuorum(makePrepareGen(qSetHash, A3, &A3,
                                                              2, 3));
                                    REQUIRE(scp.mEnvs.size() == 9);
                                    verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 3);

                                    REQUIRE(scp.mExternalizedValues.size() ==
                                            0);

                                    SECTION("Quorum externalize A3")
                                    {
                                        recvQuorum(makeConfirmGen(qSetHash, 3,
                                                                  A3, 2, 3));
                                        REQUIRE(scp.mEnvs.size() == 10);
                                        verifyExternalize(scp.mEnvs[9],
                                                          v0SecretKey,
                                                          qSetHash0, 0, A2, 3);

                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            1);
                                        REQUIRE(scp.mExternalizedValues[0] ==
                                                aValue);
                                    }
                                }
                                SECTION("v-blocking accept more A3")
                                {
                                    SECTION("Confirm A3")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 3, A3, 2, 3));
                                        REQUIRE(scp.mEnvs.size() == 9);
                                        verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                      qSetHash0, 0, 3, A3, 2,
                                                      3);
                                    }
                                    SECTION("Externalize A3")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, A2, 3));
                                        REQUIRE(scp.mEnvs.size() == 9);
                                        verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      AInf, 2, UINT32_MAX);
                                    }
                                    SECTION("other nodes moved to c=A4 h=A5")
                                    {
                                        SECTION("Confirm A4..5")
                                        {
                                            recvVBlocking(makeConfirmGen(
                                                qSetHash, 3, A5, 4, 5));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, 3, A5, 4, 5);
                                        }
                                        SECTION("Externalize A4..5")
                                        {
                                            recvVBlocking(makeExternalizeGen(
                                                qSetHash, A4, 5));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, UINT32_MAX, AInf,
                                                4, UINT32_MAX);
                                        }
                                    }
                                }
                            }
                            SECTION("v-blocking prepared A3")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, A3, &A3, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);
                            }
                            SECTION("v-blocking prepared A3+B3")
                            {
                                recvVBlocking(makePrepareGen(qSetHash, A3, &B3,
                                                             2, 2, &A3));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);
                            }
                            SECTION("v-blocking confirm A3")
                            {
                                recvVBlocking(
                                    makeConfirmGen(qSetHash, 3, A3, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);
                            }
                            SECTION("Hang - does not switch to B in CONFIRM")
                            {
                                SECTION("Network EXTERNALIZE")
                                {
                                    // externalize messages have a counter at
                                    // infinite
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, B2, 3));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 2, AInf, 2, 2);
                                    // stuck
                                    recvQuorumChecks(
                                        makeExternalizeGen(qSetHash, B2, 3),
                                        false, false);
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    REQUIRE(scp.mExternalizedValues.size() ==
                                            0);
                                }
                                SECTION("Network CONFIRMS other ballot")
                                {
                                    SECTION("at same counter")
                                    {
                                        // nothing should happen here, in
                                        // particular, node should not attempt
                                        // to switch 'p'
                                        recvQuorumChecks(
                                            makeConfirmGen(qSetHash, 3, B2, 2,
                                                           3),
                                            false, false);
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);
                                    }
                                    SECTION("at a different counter")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 3, B3, 3, 3));
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                      qSetHash0, 0, 2, A3, 2,
                                                      2);

                                        recvQuorumChecks(
                                            makeConfirmGen(qSetHash, 3, B3, 3,
                                                           3),
                                            false, false);
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);
                                    }
                                }
                            }
                        }
                        SECTION("v-blocking")
                        {
                            SECTION("CONFIRM")
                            {
                                SECTION("CONFIRM A2")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 2, A2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, 2, A2, 2, 2);
                                }
                                SECTION("CONFIRM A3..4")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 4, A4, 3, 4));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, 4, A4, 3, 4);
                                }
                                SECTION("CONFIRM B2")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 2, B2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, 2, B2, 2, 2);
                                }
                            }
                            SECTION("EXTERNALIZE")
                            {
                                SECTION("EXTERNALIZE A2")
                                {
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, A2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, UINT32_MAX,
                                                  AInf, 2, UINT32_MAX);
                                }
                                SECTION("EXTERNALIZE B2")
                                {
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, B2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, UINT32_MAX,
                                                  BInf, 2, UINT32_MAX);
                                }
                            }
                        }
                    }
                    SECTION("get conflicting prepared B")
                    {
                        SECTION("same counter")
                        {
                            recvVBlocking(makePrepareGen(qSetHash, B2, &B2));
                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, A2, &B2, 0, 2, &A2);
                        }
                        SECTION("higher counter")
                        {
                            recvVBlocking(
                                makePrepareGen(qSetHash, B3, &B2, 2, 2));
                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, A3, &B2, 0, 2, &A2);

                            recvQuorumChecks(
                                makePrepareGen(qSetHash, B3, &B2, 2, 2), true,
                                true);
                            REQUIRE(scp.mEnvs.size() == 7);
                            verifyConfirm(scp.mEnvs[6], v0SecretKey, qSetHash0,
                                          0, 3, B3, 2, 2);
                        }
                    }
                }
                SECTION("Confirm prepared mixed")
                {
                    // a few nodes prepared B2
                    recvVBlocking(makePrepareGen(qSetHash, B2, &B2, 0, 0, &A2));
                    REQUIRE(scp.mEnvs.size() == 5);
                    verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, A2,
                                  &B2, 0, 0, &A2);

                    SECTION("mixed A2")
                    {
                        // causes h=A2
                        // but c = 0, as p >!~ h
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, A2, &A2));

                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      A2, &B2, 0, 2, &A2);

                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, A2, &A2));

                        REQUIRE(scp.mEnvs.size() == 6);
                    }
                    SECTION("mixed B2")
                    {
                        // causes h=B2, c=B2
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, B2, &B2));

                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      B2, &B2, 2, 2, &A2);

                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, B2, &B2));

                        REQUIRE(scp.mEnvs.size() == 6);
                    }
                }
            }
            SECTION("switch prepared B1")
            {
                recvVBlocking(makePrepareGen(qSetHash, B1, &B1));
                REQUIRE(scp.mEnvs.size() == 3);
                verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A1, &B1,
                              0, 0, &A1);
            }
        }
        SECTION("prepared B (v-blocking)")
        {
            recvVBlocking(makePrepareGen(qSetHash, B1, &B1));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
        }
        SECTION("confirm (v-blocking)")
        {
            SECTION("via CONFIRM")
            {
                scp.receiveEnvelope(
                    makeConfirm(v1SecretKey, qSetHash, 0, 3, A3, 3, 3));
                scp.receiveEnvelope(
                    makeConfirm(v2SecretKey, qSetHash, 0, 4, A4, 2, 4));
                REQUIRE(scp.mEnvs.size() == 2);
                verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, 3, A3, 3,
                              3);
            }
            SECTION("via EXTERNALIZE")
            {
                scp.receiveEnvelope(
                    makeExternalize(v1SecretKey, qSetHash, 0, A2, 4));
                scp.receiveEnvelope(
                    makeExternalize(v2SecretKey, qSetHash, 0, A3, 5));
                REQUIRE(scp.mEnvs.size() == 2);
                verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0,
                              UINT32_MAX, AInf, 3, UINT32_MAX);
            }
        }
    }

    // this is the same test suite than "start <1,x>" with the exception that
    // some transitions are not possible as x < y - so instead we verify that
    // nothing happens
    SECTION("start <1,y>")
    {
        Value const& aValue = yValue;
        Value const& bValue = xValue;
        SCPBallot A1(1, aValue);
        SCPBallot B1(1, bValue);

        SCPBallot A2 = A1;
        A2.counter++;

        SCPBallot A3 = A2;
        A3.counter++;

        SCPBallot A4 = A3;
        A4.counter++;

        SCPBallot A5 = A4;
        A5.counter++;

        SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

        SCPBallot B2 = B1;
        B2.counter++;

        SCPBallot B3 = B2;
        B3.counter++;

        REQUIRE(scp.bumpState(0, aValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SECTION("prepared A1")
        {
            recvQuorum(makePrepareGen(qSetHash, A1));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &A1);

            SECTION("bump prepared A2")
            {
                // bump to (2,a)

                REQUIRE(scp.bumpState(0, aValue));
                REQUIRE(scp.mEnvs.size() == 3);
                verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A2, &A1);

                recvQuorum(makePrepareGen(qSetHash, A2));
                REQUIRE(scp.mEnvs.size() == 4);
                verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A2, &A2);

                SECTION("Confirm prepared A2")
                {
                    recvQuorum(makePrepareGen(qSetHash, A2, &A2));
                    REQUIRE(scp.mEnvs.size() == 5);
                    verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, A2,
                                  &A2, 2, 2);

                    SECTION("Accept commit")
                    {
                        SECTION("Quorum A2")
                        {
                            recvQuorum(makePrepareGen(qSetHash, A2, &A2, 2, 2));
                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyConfirm(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, 2, A2, 2, 2);

                            SECTION("Quorum prepared A3")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, A3, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 2, A3, 2, 2);

                                recvQuorum(
                                    makePrepareGen(qSetHash, A3, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 8);
                                verifyConfirm(scp.mEnvs[7], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);

                                SECTION("Accept more commit A3")
                                {
                                    recvQuorum(makePrepareGen(qSetHash, A3, &A3,
                                                              2, 3));
                                    REQUIRE(scp.mEnvs.size() == 9);
                                    verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 3);

                                    REQUIRE(scp.mExternalizedValues.size() ==
                                            0);

                                    SECTION("Quorum externalize A3")
                                    {
                                        recvQuorum(makeConfirmGen(qSetHash, 3,
                                                                  A3, 2, 3));
                                        REQUIRE(scp.mEnvs.size() == 10);
                                        verifyExternalize(scp.mEnvs[9],
                                                          v0SecretKey,
                                                          qSetHash0, 0, A2, 3);

                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            1);
                                        REQUIRE(scp.mExternalizedValues[0] ==
                                                aValue);
                                    }
                                }
                                SECTION("v-blocking accept more A3")
                                {
                                    SECTION("Confirm A3")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 3, A3, 2, 3));
                                        REQUIRE(scp.mEnvs.size() == 9);
                                        verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                      qSetHash0, 0, 3, A3, 2,
                                                      3);
                                    }
                                    SECTION("Externalize A3")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, A2, 3));
                                        REQUIRE(scp.mEnvs.size() == 9);
                                        verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      AInf, 2, UINT32_MAX);
                                    }
                                    SECTION("other nodes moved to c=A4 h=A5")
                                    {
                                        SECTION("Confirm A4..5")
                                        {
                                            recvVBlocking(makeConfirmGen(
                                                qSetHash, 3, A5, 4, 5));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, 3, A5, 4, 5);
                                        }
                                        SECTION("Externalize A4..5")
                                        {
                                            recvVBlocking(makeExternalizeGen(
                                                qSetHash, A4, 5));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, UINT32_MAX, AInf,
                                                4, UINT32_MAX);
                                        }
                                    }
                                }
                            }
                            SECTION("v-blocking prepared A3")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, A3, &A3, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);
                            }
                            SECTION("v-blocking prepared A3+B3")
                            {
                                recvVBlocking(makePrepareGen(qSetHash, A3, &A3,
                                                             2, 2, &B3));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);
                            }
                            SECTION("v-blocking confirm A3")
                            {
                                recvVBlocking(
                                    makeConfirmGen(qSetHash, 3, A3, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, A3, 2, 2);
                            }
                            SECTION("Hang - does not switch to B in CONFIRM")
                            {
                                SECTION("Network EXTERNALIZE")
                                {
                                    // externalize messages have a counter at
                                    // infinite
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, B2, 3));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 2, AInf, 2, 2);
                                    // stuck
                                    recvQuorumChecks(
                                        makeExternalizeGen(qSetHash, B2, 3),
                                        false, false);
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    REQUIRE(scp.mExternalizedValues.size() ==
                                            0);
                                }
                                SECTION("Network CONFIRMS other ballot")
                                {
                                    SECTION("at same counter")
                                    {
                                        // nothing should happen here, in
                                        // particular, node should not attempt
                                        // to switch 'p'
                                        recvQuorumChecks(
                                            makeConfirmGen(qSetHash, 3, B2, 2,
                                                           3),
                                            false, false);
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);
                                    }
                                    SECTION("at a different counter")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 3, B3, 3, 3));
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                      qSetHash0, 0, 2, A3, 2,
                                                      2);

                                        recvQuorumChecks(
                                            makeConfirmGen(qSetHash, 3, B3, 3,
                                                           3),
                                            false, false);
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);
                                    }
                                }
                            }
                        }
                        SECTION("v-blocking")
                        {
                            SECTION("CONFIRM")
                            {
                                SECTION("CONFIRM A2")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 2, A2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, 2, A2, 2, 2);
                                }
                                SECTION("CONFIRM A3..4")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 4, A4, 3, 4));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, 4, A4, 3, 4);
                                }
                                SECTION("CONFIRM B2")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 2, B2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, 2, B2, 2, 2);
                                }
                            }
                            SECTION("EXTERNALIZE")
                            {
                                SECTION("EXTERNALIZE A2")
                                {
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, A2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, UINT32_MAX,
                                                  AInf, 2, UINT32_MAX);
                                }
                                SECTION("EXTERNALIZE B2")
                                {
                                    // can switch to B2 with externalize (higher
                                    // counter)
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, B2, 2));
                                    REQUIRE(scp.mEnvs.size() == 6);
                                    verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                  qSetHash0, 0, UINT32_MAX,
                                                  BInf, 2, UINT32_MAX);
                                }
                            }
                        }
                    }
                    SECTION("get conflicting prepared B")
                    {
                        SECTION("same counter")
                        {
                            // messages are ignored as B2 < A2
                            recvQuorumChecks(makePrepareGen(qSetHash, B2, &B2),
                                             false, false);
                            REQUIRE(scp.mEnvs.size() == 5);
                        }
                        SECTION("higher counter")
                        {
                            recvVBlocking(
                                makePrepareGen(qSetHash, B3, &B2, 2, 2));
                            REQUIRE(scp.mEnvs.size() == 6);
                            // A2 > B2 -> p = A2, p'=B2
                            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, A3, &A2, 2, 2, &B2);

                            // node is trying to commit A2=<2,y> but rest
                            // of its quorum is trying to commit B2
                            // we end up with a delayed quorum
                            recvQuorumChecks(
                                makePrepareGen(qSetHash, B3, &B2, 2, 2), true,
                                true);
                            REQUIRE(scp.mEnvs.size() == 7);
                            verifyConfirm(scp.mEnvs[6], v0SecretKey, qSetHash0,
                                          0, 3, B3, 2, 2);
                        }
                    }
                }
                SECTION("Confirm prepared mixed")
                {
                    // a few nodes prepared B2
                    recvVBlocking(makePrepareGen(qSetHash, A2, &A2, 0, 0, &B2));
                    REQUIRE(scp.mEnvs.size() == 5);
                    verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, A2,
                                  &A2, 0, 0, &B2);

                    SECTION("mixed A2")
                    {
                        // causes h=A2, c=A2
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, A2, &A2));

                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      A2, &A2, 2, 2, &B2);

                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, A2, &A2));

                        REQUIRE(scp.mEnvs.size() == 6);
                    }
                    SECTION("mixed B2")
                    {
                        // causes h=B2
                        // but c = 0, as p >!~ h
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, A2, &B2));

                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      A2, &A2, 0, 2, &B2);

                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, B2, &B2));

                        REQUIRE(scp.mEnvs.size() == 6);
                    }
                }
            }
            SECTION("switch prepared B1")
            {
                // can't switch to B1
                recvQuorumChecks(makePrepareGen(qSetHash, B1, &B1), false,
                                 false);
                REQUIRE(scp.mEnvs.size() == 2);
            }
        }
        SECTION("prepared B (v-blocking)")
        {
            recvVBlocking(makePrepareGen(qSetHash, B1, &B1));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
        }
        SECTION("confirm (v-blocking)")
        {
            SECTION("via CONFIRM")
            {
                scp.receiveEnvelope(
                    makeConfirm(v1SecretKey, qSetHash, 0, 3, A3, 3, 3));
                scp.receiveEnvelope(
                    makeConfirm(v2SecretKey, qSetHash, 0, 4, A4, 2, 4));
                REQUIRE(scp.mEnvs.size() == 2);
                verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, 3, A3, 3,
                              3);
            }
            SECTION("via EXTERNALIZE")
            {
                scp.receiveEnvelope(
                    makeExternalize(v1SecretKey, qSetHash, 0, A2, 4));
                scp.receiveEnvelope(
                    makeExternalize(v2SecretKey, qSetHash, 0, A3, 5));
                REQUIRE(scp.mEnvs.size() == 2);
                verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0,
                              UINT32_MAX, AInf, 3, UINT32_MAX);
            }
        }
    }

    // this is the same test suite than "start <1,x>" but only keeping
    // the transitions that are observable when starting from empty
    SECTION("start from pristine")
    {
        Value const& aValue = xValue;
        Value const& bValue = yValue;

        SCPBallot A1(1, aValue);
        SCPBallot B1(1, bValue);

        SCPBallot A2 = A1;
        A2.counter++;

        SCPBallot A3 = A2;
        A3.counter++;

        SCPBallot A4 = A3;
        A4.counter++;

        SCPBallot A5 = A4;
        A5.counter++;

        SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

        SCPBallot B2 = B1;
        B2.counter++;

        SCPBallot B3 = B2;
        B3.counter++;

        REQUIRE(scp.mEnvs.size() == 0);

        SECTION("prepared A1")
        {
            recvQuorumChecks(makePrepareGen(qSetHash, A1), false, false);
            REQUIRE(scp.mEnvs.size() == 0);

            SECTION("bump prepared A2")
            {
                SECTION("Confirm prepared A2")
                {
                    recvVBlockingChecks(makePrepareGen(qSetHash, A2, &A2),
                                        false);
                    REQUIRE(scp.mEnvs.size() == 0);

                    SECTION("Quorum A2")
                    {
                        recvVBlockingChecks(makePrepareGen(qSetHash, A2, &A2),
                                            false);
                        REQUIRE(scp.mEnvs.size() == 0);
                        recvQuorum(makePrepareGen(qSetHash, A2, &A2));
                        REQUIRE(scp.mEnvs.size() == 1);
                        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                                      A2, &A2, 1, 2);
                    }
                    SECTION("Quorum B2")
                    {
                        recvVBlockingChecks(makePrepareGen(qSetHash, B2, &B2),
                                            false);
                        REQUIRE(scp.mEnvs.size() == 0);
                        recvQuorum(makePrepareGen(qSetHash, B2, &B2));
                        REQUIRE(scp.mEnvs.size() == 1);
                        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                                      B2, &B2, 2, 2, &A2);
                    }
                    SECTION("Accept commit")
                    {
                        SECTION("Quorum A2")
                        {
                            recvQuorum(makePrepareGen(qSetHash, A2, &A2, 2, 2));
                            REQUIRE(scp.mEnvs.size() == 1);
                            verifyConfirm(scp.mEnvs[0], v0SecretKey, qSetHash0,
                                          0, 2, A2, 2, 2);
                        }
                        SECTION("Quorum B2")
                        {
                            recvQuorum(makePrepareGen(qSetHash, B2, &B2, 2, 2));
                            REQUIRE(scp.mEnvs.size() == 1);
                            verifyConfirm(scp.mEnvs[0], v0SecretKey, qSetHash0,
                                          0, 2, B2, 2, 2);
                        }
                        SECTION("v-blocking")
                        {
                            SECTION("CONFIRM")
                            {
                                SECTION("CONFIRM A2")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 2, A2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 1);
                                    verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                  qSetHash0, 0, 2, A2, 2, 2);
                                }
                                SECTION("CONFIRM A3..4")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 4, A4, 3, 4));
                                    REQUIRE(scp.mEnvs.size() == 1);
                                    verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                  qSetHash0, 0, 4, A4, 3, 4);
                                }
                                SECTION("CONFIRM B2")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 2, B2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 1);
                                    verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                  qSetHash0, 0, 2, B2, 2, 2);
                                }
                            }
                            SECTION("EXTERNALIZE")
                            {
                                SECTION("EXTERNALIZE A2")
                                {
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, A2, 2));
                                    REQUIRE(scp.mEnvs.size() == 1);
                                    verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                  qSetHash0, 0, UINT32_MAX,
                                                  AInf, 2, UINT32_MAX);
                                }
                                SECTION("EXTERNALIZE B2")
                                {
                                    recvVBlocking(
                                        makeExternalizeGen(qSetHash, B2, 2));
                                    REQUIRE(scp.mEnvs.size() == 1);
                                    verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                  qSetHash0, 0, UINT32_MAX,
                                                  BInf, 2, UINT32_MAX);
                                }
                            }
                        }
                    }
                }
                SECTION("Confirm prepared mixed")
                {
                    // a few nodes prepared A2
                    // causes p=A2
                    recvVBlockingChecks(makePrepareGen(qSetHash, A2, &A2),
                                        false);
                    REQUIRE(scp.mEnvs.size() == 0);

                    // a few nodes prepared B2
                    // causes p=B2, p'=A2
                    recvVBlockingChecks(
                        makePrepareGen(qSetHash, A2, &B2, 0, 0, &A2), false);
                    REQUIRE(scp.mEnvs.size() == 0);

                    SECTION("mixed A2")
                    {
                        // causes h=A2
                        // but c = 0, as p >!~ h
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, A2, &A2));

                        REQUIRE(scp.mEnvs.size() == 1);
                        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                                      A2, &B2, 0, 2, &A2);

                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, A2, &A2));

                        REQUIRE(scp.mEnvs.size() == 1);
                    }
                    SECTION("mixed B2")
                    {
                        // causes h=B2, c=B2
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, B2, &B2));

                        REQUIRE(scp.mEnvs.size() == 1);
                        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                                      B2, &B2, 2, 2, &A2);

                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, B2, &B2));

                        REQUIRE(scp.mEnvs.size() == 1);
                    }
                }
            }
            SECTION("switch prepared B1")
            {
                recvVBlockingChecks(makePrepareGen(qSetHash, B1, &B1), false);
                REQUIRE(scp.mEnvs.size() == 0);
            }
        }
        SECTION("prepared B (v-blocking)")
        {
            recvVBlockingChecks(makePrepareGen(qSetHash, B1, &B1), false);
            REQUIRE(scp.mEnvs.size() == 0);
        }
        SECTION("confirm (v-blocking)")
        {
            SECTION("via CONFIRM")
            {
                scp.receiveEnvelope(
                    makeConfirm(v1SecretKey, qSetHash, 0, 3, A3, 3, 3));
                scp.receiveEnvelope(
                    makeConfirm(v2SecretKey, qSetHash, 0, 4, A4, 2, 4));
                REQUIRE(scp.mEnvs.size() == 1);
                verifyConfirm(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, 3, A3, 3,
                              3);
            }
            SECTION("via EXTERNALIZE")
            {
                scp.receiveEnvelope(
                    makeExternalize(v1SecretKey, qSetHash, 0, A2, 4));
                scp.receiveEnvelope(
                    makeExternalize(v2SecretKey, qSetHash, 0, A3, 5));
                REQUIRE(scp.mEnvs.size() == 1);
                verifyConfirm(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                              UINT32_MAX, AInf, 3, UINT32_MAX);
            }
        }
    }

    SECTION("normal round (1,x)")
    {
        nodesAllPledgeToCommit();
        REQUIRE(scp.mEnvs.size() == 3);

        SCPBallot b(1, xValue);

        // bunch of prepare messages with "commit b"
        SCPEnvelope preparedC1 =
            makePrepare(v1SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC2 =
            makePrepare(v2SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC3 =
            makePrepare(v3SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC4 =
            makePrepare(v4SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);

        // those should not trigger anything just yet
        scp.receiveEnvelope(preparedC1);
        scp.receiveEnvelope(preparedC2);
        REQUIRE(scp.mEnvs.size() == 3);

        // this should cause the node to accept 'commit b' (quorum)
        // and therefore send a "CONFIRM" message
        scp.receiveEnvelope(preparedC3);
        REQUIRE(scp.mEnvs.size() == 4);

        verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 1, b, b.counter,
                      b.counter);

        // bunch of confirm messages
        SCPEnvelope confirm1 = makeConfirm(v1SecretKey, qSetHash, 0, b.counter,
                                           b, b.counter, b.counter);
        SCPEnvelope confirm2 = makeConfirm(v2SecretKey, qSetHash, 0, b.counter,
                                           b, b.counter, b.counter);
        SCPEnvelope confirm3 = makeConfirm(v3SecretKey, qSetHash, 0, b.counter,
                                           b, b.counter, b.counter);
        SCPEnvelope confirm4 = makeConfirm(v4SecretKey, qSetHash, 0, b.counter,
                                           b, b.counter, b.counter);

        // those should not trigger anything just yet
        scp.receiveEnvelope(confirm1);
        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == 4);

        scp.receiveEnvelope(confirm3);
        // this causes our node to
        // externalize (confirm commit c)
        REQUIRE(scp.mEnvs.size() == 5);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues.size() == 1);
        REQUIRE(scp.mExternalizedValues[0] == xValue);

        verifyExternalize(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, b,
                          b.counter);

        // extra vote should not do anything
        scp.receiveEnvelope(confirm4);
        REQUIRE(scp.mEnvs.size() == 5);
        REQUIRE(scp.mExternalizedValues.size() == 1);

        // duplicate should just no-op
        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == 5);
        REQUIRE(scp.mExternalizedValues.size() == 1);

        SECTION("bumpToBallot prevented once committed")
        {
            SCPBallot b2;
            SECTION("bumpToBallot prevented once committed (by value)")
            {
                b2 = SCPBallot(1, yValue);
            }
            SECTION("bumpToBallot prevented once committed (by counter)")
            {
                b2 = SCPBallot(2, xValue);
            }
            SECTION(
                "bumpToBallot prevented once committed (by value and counter)")
            {
                b2 = SCPBallot(2, yValue);
            }

            SCPEnvelope confirm1b2, confirm2b2, confirm3b2, confirm4b2;
            confirm1b2 = makeConfirm(v1SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter, b2.counter);
            confirm2b2 = makeConfirm(v2SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter, b2.counter);
            confirm3b2 = makeConfirm(v3SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter, b2.counter);
            confirm4b2 = makeConfirm(v4SecretKey, qSetHash, 0, b2.counter, b2,
                                     b2.counter, b2.counter);

            scp.receiveEnvelope(confirm1b2);
            scp.receiveEnvelope(confirm2b2);
            scp.receiveEnvelope(confirm3b2);
            scp.receiveEnvelope(confirm4b2);
            REQUIRE(scp.mEnvs.size() == 5);
            REQUIRE(scp.mExternalizedValues.size() == 1);
        }
    }

    SECTION("range check")
    {
        nodesAllPledgeToCommit();
        REQUIRE(scp.mEnvs.size() == 3);

        SCPBallot b(1, xValue);

        // bunch of prepare messages with "commit b"
        SCPEnvelope preparedC1 =
            makePrepare(v1SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC2 =
            makePrepare(v2SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC3 =
            makePrepare(v3SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);
        SCPEnvelope preparedC4 =
            makePrepare(v4SecretKey, qSetHash, 0, b, &b, b.counter, b.counter);

        // those should not trigger anything just yet
        scp.receiveEnvelope(preparedC1);
        scp.receiveEnvelope(preparedC2);
        REQUIRE(scp.mEnvs.size() == 3);

        // this should cause the node to accept 'commit b' (quorum)
        // and therefore send a "CONFIRM" message
        scp.receiveEnvelope(preparedC3);
        REQUIRE(scp.mEnvs.size() == 4);

        verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 1, b, b.counter,
                      b.counter);

        // bunch of confirm messages with different ranges
        SCPBallot b5(5, xValue);
        SCPEnvelope confirm1 = makeConfirm(v1SecretKey, qSetHash, 0, 4,
                                           SCPBallot(4, xValue), 2, 4);
        SCPEnvelope confirm2 = makeConfirm(v2SecretKey, qSetHash, 0, 6,
                                           SCPBallot(6, xValue), 2, 6);
        SCPEnvelope confirm3 = makeConfirm(v3SecretKey, qSetHash, 0, 5,
                                           SCPBallot(5, xValue), 3, 5);
        SCPEnvelope confirm4 = makeConfirm(v4SecretKey, qSetHash, 0, 6,
                                           SCPBallot(6, xValue), 3, 6);

        // this should not trigger anything just yet
        scp.receiveEnvelope(confirm1);

        // v-blocking
        //   * b gets bumped to (4,x)
        //   * p gets bumped to (4,x)
        //   * (c,h) gets bumped to (2,4)
        scp.receiveEnvelope(confirm2);
        REQUIRE(scp.mEnvs.size() == 5);
        verifyConfirm(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, 4,
                      SCPBallot(4, xValue), 2, 4);

        // this causes to externalize
        // range is [3,4]
        scp.receiveEnvelope(confirm4);
        REQUIRE(scp.mEnvs.size() == 6);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues.size() == 1);
        REQUIRE(scp.mExternalizedValues[0] == xValue);

        verifyExternalize(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                          SCPBallot(3, xValue), 4);
    }

    SECTION("timeout when h is set -> stay locked on h")
    {
        SCPBallot bx(1, xValue);
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        // v-blocking -> prepared
        // quorum -> confirm prepared
        recvQuorum(makePrepareGen(qSetHash, bx, &bx));
        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, bx, &bx,
                      bx.counter, bx.counter);

        // now, see if we can timeout and move to a different value
        REQUIRE(scp.bumpState(0, yValue));
        REQUIRE(scp.mEnvs.size() == 4);
        SCPBallot newbx(2, xValue);
        verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, newbx, &bx,
                      bx.counter, bx.counter);
    }

    SECTION("timeout from multiple nodes")
    {
        REQUIRE(scp.bumpState(0, xValue));

        SCPBallot x1(1, xValue);

        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);

        recvQuorum(makePrepareGen(qSetHash, x1));
        // quorum -> prepared (1,x)
        REQUIRE(scp.mEnvs.size() == 2);
        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);

        SCPBallot x2(2, xValue);
        // timeout from local node
        REQUIRE(scp.bumpState(0, xValue));
        // prepares (2,x)
        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x2, &x1);

        recvQuorum(makePrepareGen(qSetHash, x1, &x1));
        // quorum -> set nH=1
        REQUIRE(scp.mEnvs.size() == 4);
        verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, x2, &x1, 0, 1);
        REQUIRE(scp.mEnvs.size() == 4);

        recvVBlocking(makePrepareGen(qSetHash, x2, &x2, 1, 1));
        // v-blocking prepared (2,x) -> prepared (2,x)
        REQUIRE(scp.mEnvs.size() == 5);
        verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, x2, &x2, 0, 1);

        recvQuorum(makePrepareGen(qSetHash, x2, &x2, 1, 1));
        // quorum (including us) confirms (2,x) prepared -> set h=c=x2
        // we also get extra message: a quorum not including us confirms (1,x)
        // prepared
        //  -> we confirm c=h=x1
        REQUIRE(scp.mEnvs.size() == 7);
        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0, x2, &x2, 2, 2);
        verifyConfirm(scp.mEnvs[6], v0SecretKey, qSetHash0, 0, 2, x2, 1, 1);
    }

    SECTION("timeout after prepare, receive old messages to prepare")
    {
        REQUIRE(scp.bumpState(0, xValue));

        SCPBallot x1(1, xValue);

        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);

        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, x1));
        scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1));

        // quorum -> prepared (1,x)
        REQUIRE(scp.mEnvs.size() == 2);
        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);

        SCPBallot x2(2, xValue);
        // timeout from local node
        REQUIRE(scp.bumpState(0, xValue));
        // prepares (2,x)
        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x2, &x1);

        SCPBallot x3(3, xValue);
        // timeout again
        REQUIRE(scp.bumpState(0, xValue));
        // prepares (3,x)
        REQUIRE(scp.mEnvs.size() == 4);
        verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, x3, &x1);

        // other nodes moved on with x2
        scp.receiveEnvelope(
            makePrepare(v1SecretKey, qSetHash, 0, x2, &x2, 1, 2));
        scp.receiveEnvelope(
            makePrepare(v2SecretKey, qSetHash, 0, x2, &x2, 1, 2));
        // v-blocking -> prepared x2
        REQUIRE(scp.mEnvs.size() == 5);
        verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, x3, &x2);

        scp.receiveEnvelope(
            makePrepare(v3SecretKey, qSetHash, 0, x2, &x2, 1, 2));
        // quorum -> set nH=2
        REQUIRE(scp.mEnvs.size() == 6);
        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0, x3, &x2, 0, 2);
    }

    SECTION("non validator watching the network")
    {
        SIMULATION_CREATE_NODE(NV);
        TestSCP scpNV(vNVSecretKey, qSet, false);
        scpNV.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
        uint256 qSetHashNV = scpNV.mSCP.getLocalNode()->getQuorumSetHash();

        SCPBallot b(1, xValue);
        REQUIRE(scpNV.bumpState(0, xValue));
        REQUIRE(scpNV.mEnvs.size() == 0);
        verifyPrepare(scpNV.getCurrentEnvelope(0, vNVNodeID), vNVSecretKey,
                      qSetHashNV, 0, b);
        auto ext1 = makeExternalize(v1SecretKey, qSetHash, 0, b, 1);
        auto ext2 = makeExternalize(v2SecretKey, qSetHash, 0, b, 1);
        auto ext3 = makeExternalize(v3SecretKey, qSetHash, 0, b, 1);
        auto ext4 = makeExternalize(v4SecretKey, qSetHash, 0, b, 1);
        scpNV.receiveEnvelope(ext1);
        scpNV.receiveEnvelope(ext2);
        scpNV.receiveEnvelope(ext3);
        REQUIRE(scpNV.mEnvs.size() == 0);
        verifyConfirm(scpNV.getCurrentEnvelope(0, vNVNodeID), vNVSecretKey,
                      qSetHashNV, 0, UINT32_MAX, SCPBallot(UINT32_MAX, xValue),
                      1, UINT32_MAX);
        scpNV.receiveEnvelope(ext4);
        REQUIRE(scpNV.mEnvs.size() == 0);
        verifyExternalize(scpNV.getCurrentEnvelope(0, vNVNodeID), vNVSecretKey,
                          qSetHashNV, 0, b, UINT32_MAX);
        REQUIRE(scpNV.mExternalizedValues[0] == xValue);
    }

    SECTION("restore ballot protocol")
    {
        TestSCP scp2(v0SecretKey, qSet);
        scp2.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
        SCPBallot b(2, xValue);
        SECTION("prepare")
        {
            scp2.mSCP.setStateFromEnvelope(
                0, makePrepare(v0SecretKey, qSetHash0, 0, b));
        }
        SECTION("confirm")
        {
            scp2.mSCP.setStateFromEnvelope(
                0, makeConfirm(v0SecretKey, qSetHash0, 0, 2, b, 1, 2));
        }
        SECTION("externalize")
        {
            scp2.mSCP.setStateFromEnvelope(
                0, makeExternalize(v0SecretKey, qSetHash0, 0, b, 2));
        }
    }
}

TEST_CASE("nomination tests core5", "[scp][nominationprotocol]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);

    // we need 5 nodes to avoid sharing various thresholds:
    // v-blocking set size: 2
    // threshold: 4 = 3 + self or 4 others
    SCPQuorumSet qSet;
    qSet.threshold = 4;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);
    qSet.validators.push_back(v4NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    REQUIRE(xValue < yValue);

    SECTION("nomination - v0 is top")
    {
        TestSCP scp(v0SecretKey, qSet);
        uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();
        scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

        SECTION("others nominate what v0 says (x) -> prepare x")
        {
            REQUIRE(scp.nominate(0, xValue, false));

            std::vector<Value> votes, accepted;
            votes.emplace_back(xValue);

            REQUIRE(scp.mEnvs.size() == 1);
            verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, votes,
                           accepted);

            SCPEnvelope nom1 =
                makeNominate(v1SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope nom2 =
                makeNominate(v2SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope nom3 =
                makeNominate(v3SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope nom4 =
                makeNominate(v4SecretKey, qSetHash, 0, votes, accepted);

            // nothing happens yet
            scp.receiveEnvelope(nom1);
            scp.receiveEnvelope(nom2);
            REQUIRE(scp.mEnvs.size() == 1);

            // this causes 'x' to be accepted (quorum)
            scp.receiveEnvelope(nom3);
            REQUIRE(scp.mEnvs.size() == 2);

            scp.mExpectedCandidates.emplace(xValue);
            scp.mCompositeValue = xValue;

            accepted.emplace_back(xValue);
            verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, votes,
                           accepted);

            // extra message doesn't do anything
            scp.receiveEnvelope(nom4);
            REQUIRE(scp.mEnvs.size() == 2);

            SCPEnvelope acc1 =
                makeNominate(v1SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope acc2 =
                makeNominate(v2SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope acc3 =
                makeNominate(v3SecretKey, qSetHash, 0, votes, accepted);
            SCPEnvelope acc4 =
                makeNominate(v4SecretKey, qSetHash, 0, votes, accepted);

            // nothing happens yet
            scp.receiveEnvelope(acc1);
            scp.receiveEnvelope(acc2);
            REQUIRE(scp.mEnvs.size() == 2);

            scp.mCompositeValue = xValue;
            // this causes the node to send a prepare message (quorum)
            scp.receiveEnvelope(acc3);
            REQUIRE(scp.mEnvs.size() == 3);

            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0,
                          SCPBallot(1, xValue));

            scp.receiveEnvelope(acc4);
            REQUIRE(scp.mEnvs.size() == 3);

            std::vector<Value> votes2 = votes;
            votes2.emplace_back(yValue);

            SECTION("nominate x -> accept x -> prepare (x) ; others accepted y "
                    "-> update latest to (z=x+y)")
            {
                SCPEnvelope acc1_2 =
                    makeNominate(v1SecretKey, qSetHash, 0, votes2, votes2);
                SCPEnvelope acc2_2 =
                    makeNominate(v2SecretKey, qSetHash, 0, votes2, votes2);
                SCPEnvelope acc3_2 =
                    makeNominate(v3SecretKey, qSetHash, 0, votes2, votes2);
                SCPEnvelope acc4_2 =
                    makeNominate(v4SecretKey, qSetHash, 0, votes2, votes2);

                scp.receiveEnvelope(acc1_2);
                REQUIRE(scp.mEnvs.size() == 3);

                // v-blocking
                scp.receiveEnvelope(acc2_2);
                REQUIRE(scp.mEnvs.size() == 4);
                verifyNominate(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, votes2,
                               votes2);

                scp.mExpectedCandidates.insert(yValue);
                scp.mCompositeValue = zValue;
                // this updates the composite value to use next time
                // but does not prepare it
                scp.receiveEnvelope(acc3_2);
                REQUIRE(scp.mEnvs.size() == 4);

                REQUIRE(scp.getLatestCompositeCandidate(0) == zValue);

                scp.receiveEnvelope(acc4_2);
                REQUIRE(scp.mEnvs.size() == 4);
            }
            SECTION("nomination - restored state")
            {
                TestSCP scp2(v0SecretKey, qSet);
                scp2.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

                // at this point
                // votes = { x }
                // accepted = { x }

                // tests if nomination proceeds like normal
                // nominates x
                auto nominationRestore = [&]() {
                    // restores from the previous state
                    scp2.mSCP.setStateFromEnvelope(
                        0, makeNominate(v0SecretKey, qSetHash0, 0, votes,
                                        accepted));
                    // tries to start nomination with yValue
                    REQUIRE(scp2.nominate(0, yValue, false));

                    REQUIRE(scp2.mEnvs.size() == 1);
                    verifyNominate(scp2.mEnvs[0], v0SecretKey, qSetHash0, 0,
                                   votes2, accepted);

                    // other nodes vote for 'x'
                    scp2.receiveEnvelope(nom1);
                    scp2.receiveEnvelope(nom2);
                    REQUIRE(scp2.mEnvs.size() == 1);
                    // 'x' is accepted (quorum)
                    // but because the restored state already included
                    // 'x' in the accepted set, no new message is emitted
                    scp2.receiveEnvelope(nom3);

                    scp2.mExpectedCandidates.emplace(xValue);
                    scp2.mCompositeValue = xValue;

                    // other nodes not emit 'x' as accepted
                    scp2.receiveEnvelope(acc1);
                    scp2.receiveEnvelope(acc2);
                    REQUIRE(scp2.mEnvs.size() == 1);

                    scp2.mCompositeValue = xValue;
                    // this causes the node to update its composite value to x
                    scp2.receiveEnvelope(acc3);
                };

                SECTION("ballot protocol not started")
                {
                    nominationRestore();
                    // nomination ended up starting the ballot protocol
                    REQUIRE(scp2.mEnvs.size() == 2);

                    verifyPrepare(scp2.mEnvs[1], v0SecretKey, qSetHash0, 0,
                                  SCPBallot(1, xValue));
                }
                SECTION("ballot protocol started (on value z)")
                {
                    scp2.mSCP.setStateFromEnvelope(
                        0, makePrepare(v0SecretKey, qSetHash0, 0,
                                       SCPBallot(1, zValue)));
                    nominationRestore();
                    // nomination didn't do anything (already working on z)
                    REQUIRE(scp2.mEnvs.size() == 1);
                }
            }
        }
        SECTION("self nominates 'x', others nominate y -> prepare y")
        {
            std::vector<Value> myVotes, accepted;
            myVotes.emplace_back(xValue);

            scp.mExpectedCandidates.emplace(xValue);
            scp.mCompositeValue = xValue;
            REQUIRE(scp.nominate(0, xValue, false));

            REQUIRE(scp.mEnvs.size() == 1);
            verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, myVotes,
                           accepted);

            std::vector<Value> votes;
            votes.emplace_back(yValue);

            std::vector<Value> acceptedY = accepted;

            acceptedY.emplace_back(yValue);

            SECTION("others only vote for y")
            {
                SCPEnvelope nom1 =
                    makeNominate(v1SecretKey, qSetHash, 0, votes, accepted);
                SCPEnvelope nom2 =
                    makeNominate(v2SecretKey, qSetHash, 0, votes, accepted);
                SCPEnvelope nom3 =
                    makeNominate(v3SecretKey, qSetHash, 0, votes, accepted);
                SCPEnvelope nom4 =
                    makeNominate(v4SecretKey, qSetHash, 0, votes, accepted);

                // nothing happens yet
                scp.receiveEnvelope(nom1);
                scp.receiveEnvelope(nom2);
                scp.receiveEnvelope(nom3);
                REQUIRE(scp.mEnvs.size() == 1);

                // 'y' is accepted (quorum)
                scp.receiveEnvelope(nom4);
                REQUIRE(scp.mEnvs.size() == 2);
                myVotes.emplace_back(yValue);
                verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, myVotes,
                               acceptedY);
            }
            SECTION("others accepted y")
            {
                SCPEnvelope acc1 =
                    makeNominate(v1SecretKey, qSetHash, 0, votes, acceptedY);
                SCPEnvelope acc2 =
                    makeNominate(v2SecretKey, qSetHash, 0, votes, acceptedY);
                SCPEnvelope acc3 =
                    makeNominate(v3SecretKey, qSetHash, 0, votes, acceptedY);
                SCPEnvelope acc4 =
                    makeNominate(v4SecretKey, qSetHash, 0, votes, acceptedY);

                scp.receiveEnvelope(acc1);
                REQUIRE(scp.mEnvs.size() == 1);

                // this causes 'y' to be accepted (v-blocking)
                scp.receiveEnvelope(acc2);
                REQUIRE(scp.mEnvs.size() == 2);

                myVotes.emplace_back(yValue);
                verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, myVotes,
                               acceptedY);

                scp.mExpectedCandidates.clear();
                scp.mExpectedCandidates.insert(yValue);
                scp.mCompositeValue = yValue;
                // this causes the node to send a prepare message (quorum)
                scp.receiveEnvelope(acc3);
                REQUIRE(scp.mEnvs.size() == 3);

                verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0,
                              SCPBallot(1, yValue));

                scp.receiveEnvelope(acc4);
                REQUIRE(scp.mEnvs.size() == 3);
            }
        }
    }
    SECTION("v1 is top node")
    {
        TestSCP scp(v0SecretKey, qSet);
        uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();
        scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

        scp.mPriorityLookup = [&](NodeID const& n) {
            return (n == v1NodeID) ? 1000 : 1;
        };

        std::vector<Value> votesX, votesY, votesZ, votesXY, votesYZ, votesXZ,
            emptyV;
        votesX.emplace_back(xValue);
        votesY.emplace_back(yValue);
        votesZ.emplace_back(zValue);

        votesXY.emplace_back(xValue);
        votesXY.emplace_back(yValue);

        votesYZ.emplace_back(yValue);
        votesYZ.emplace_back(zValue);

        votesXZ.emplace_back(xValue);
        votesXZ.emplace_back(zValue);

        std::vector<Value> valuesHash;
        valuesHash.emplace_back(xValue);
        valuesHash.emplace_back(yValue);
        valuesHash.emplace_back(zValue);

        scp.mHashValueCalculator = [&](Value const& v) {
            auto pos = std::find(valuesHash.begin(), valuesHash.end(), v);
            if (pos == valuesHash.end())
            {
                abort();
            }
            return 1 + std::distance(valuesHash.begin(), pos);
        };

        SCPEnvelope nom1 =
            makeNominate(v1SecretKey, qSetHash, 0, votesXY, emptyV);
        SCPEnvelope nom2 =
            makeNominate(v2SecretKey, qSetHash, 0, votesXZ, emptyV);

        SECTION("nomination waits for v1")
        {
            REQUIRE(!scp.nominate(0, xValue, false));

            REQUIRE(scp.mEnvs.size() == 0);

            SCPEnvelope nom3 =
                makeNominate(v3SecretKey, qSetHash, 0, votesYZ, emptyV);
            SCPEnvelope nom4 =
                makeNominate(v4SecretKey, qSetHash, 0, votesXZ, emptyV);

            // nothing happens with non top nodes
            scp.receiveEnvelope(nom2);
            scp.receiveEnvelope(nom3);
            REQUIRE(scp.mEnvs.size() == 0);

            scp.receiveEnvelope(nom1);
            REQUIRE(scp.mEnvs.size() == 1);
            verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, votesY,
                           emptyV);

            scp.receiveEnvelope(nom4);
            REQUIRE(scp.mEnvs.size() == 1);

            SECTION("timeout -> pick another value from v1")
            {
                scp.mExpectedCandidates.emplace(xValue);
                scp.mCompositeValue = xValue;

                // note: value passed in here should be ignored
                REQUIRE(scp.nominate(0, zValue, true));
                // picks up 'x' from v1 (as we already have 'y')
                // which also happens to causes 'x' to be accepted
                REQUIRE(scp.mEnvs.size() == 2);
                verifyNominate(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, votesXY,
                               votesX);
            }
        }
        SECTION("v1 dead, timeout")
        {
            REQUIRE(!scp.nominate(0, xValue, false));

            REQUIRE(scp.mEnvs.size() == 0);

            scp.receiveEnvelope(nom2);
            REQUIRE(scp.mEnvs.size() == 0);

            SECTION("v0 is new top node")
            {
                scp.mPriorityLookup = [&](NodeID const& n) {
                    return (n == v0NodeID) ? 1000 : 1;
                };

                REQUIRE(scp.nominate(0, xValue, true));
                REQUIRE(scp.mEnvs.size() == 1);
                verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, votesX,
                               emptyV);
            }
            SECTION("v2 is new top node")
            {
                scp.mPriorityLookup = [&](NodeID const& n) {
                    return (n == v2NodeID) ? 1000 : 1;
                };

                REQUIRE(scp.nominate(0, xValue, true));
                REQUIRE(scp.mEnvs.size() == 1);
                verifyNominate(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, votesZ,
                               emptyV);
            }
            SECTION("v3 is new top node")
            {
                scp.mPriorityLookup = [&](NodeID const& n) {
                    return (n == v3NodeID) ? 1000 : 1;
                };
                // nothing happens, we don't have any message for v3
                REQUIRE(!scp.nominate(0, xValue, true));
                REQUIRE(scp.mEnvs.size() == 0);
            }
        }
    }
}
}
