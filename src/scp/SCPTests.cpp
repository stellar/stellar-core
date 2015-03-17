// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"

#include "lib/catch.hpp"
#include "scp/SCP.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"

using namespace stellar;

using xdr::operator<;
using xdr::operator==;

class TestSCP : public SCP
{
  public:
    TestSCP(const SecretKey& secretKey, const SCPQuorumSet& qSetLocal)
        : SCP(secretKey, qSetLocal)
    {
    }
    void
    storeQuorumSet(SCPQuorumSet qSet)
    {
        Hash qSetHash = sha256(xdr::xdr_to_opaque(qSet));
        mQuorumSets[qSetHash] = qSet;
    }

    void
    validateValue(const uint64& slotIndex, const Hash& nodeID,
                  const Value& value, std::function<void(bool)> const& cb)
    {
        cb(true);
    }

    void
    validateBallot(const uint64& slotIndex, const Hash& nodeID,
                   const SCPBallot& ballot, std::function<void(bool)> const& cb)
    {
        cb(true);
    }

    void
    ballotDidPrepare(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    void
    ballotDidPrepared(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    void
    ballotDidCommit(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    void
    ballotDidCommitted(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    void
    ballotDidHearFromQuorum(const uint64& slotIndex, const SCPBallot& ballot)
    {
        mHeardFromQuorums[slotIndex].push_back(ballot);
    }

    void
    valueExternalized(const uint64& slotIndex, const Value& value)
    {
        if (mExternalizedValues.find(slotIndex) != mExternalizedValues.end())
        {
            throw std::out_of_range("Value already externalized");
        }
        mExternalizedValues[slotIndex] = value;
    }

    void
    retrieveQuorumSet(const Hash& nodeID, const Hash& qSetHash,
                      std::function<void(const SCPQuorumSet&)> const& cb)
    {
        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {
            CLOG(DEBUG, "SCP") << "TestSCP::retrieveQuorumSet"
                               << " qSet: " << binToHex(qSetHash).substr(0, 6)
                               << " nodeID: " << binToHex(nodeID).substr(0, 6)
                               << " OK";
            return cb(mQuorumSets[qSetHash]);
        }
        else
        {
            CLOG(DEBUG, "SCP") << "TestSCP::retrieveQuorumSet"
                               << " qSet: " << binToHex(qSetHash).substr(0, 6)
                               << " nodeID: " << binToHex(nodeID).substr(0, 6)
                               << " FAIL";
        }
    }
    void
    emitEnvelope(const SCPEnvelope& envelope)
    {
        mEnvs.push_back(envelope);
    }

    std::map<Hash, SCPQuorumSet> mQuorumSets;
    std::vector<SCPEnvelope> mEnvs;
    std::map<uint64, Value> mExternalizedValues;
    std::map<uint64, std::vector<SCPBallot>> mHeardFromQuorums;
};

static SCPEnvelope
makeEnvelope(const SecretKey& secretKey, const Hash& qSetHash,
             const uint64& slotIndex, const SCPBallot& ballot,
             const SCPStatementType& type)
{
    SCPEnvelope envelope;

    envelope.nodeID = secretKey.getPublicKey();
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.ballot = ballot;
    envelope.statement.quorumSetHash = qSetHash;
    envelope.statement.pledges.type(type);
    envelope.signature = secretKey.sign(xdr::xdr_to_opaque(envelope.statement));

    return envelope;
}

void
signEnvelope(const SecretKey& secretKey, SCPEnvelope& envelope)
{
    envelope.signature = secretKey.sign(xdr::xdr_to_opaque(envelope.statement));
}

#define CREATE_VALUE(X)                                                        \
    const Hash X##ValueHash = sha256("SEED_VALUE_HASH_" #X);                   \
    const Value X##Value = xdr::xdr_to_opaque(X##ValueHash);

TEST_CASE("protocol core4", "[scp]")
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

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey, qSet);

    scp.storeQuorumSet(qSet);

    CREATE_VALUE(x);
    CREATE_VALUE(y);
    CREATE_VALUE(z);

    REQUIRE(xValue < yValue);

    CLOG(INFO, "SCP") << "";
    CLOG(INFO, "SCP") << "BEGIN TEST";

    SECTION("prepareValue x")
    {
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.slotIndex == 0);
        REQUIRE(scp.mEnvs[0].statement.quorumSetHash == qSetHash);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);
        REQUIRE(!scp.mEnvs[0].statement.pledges.prepare().prepared);
        REQUIRE(scp.mEnvs[0].statement.pledges.prepare().excepted.size() == 0);
    }

    SECTION("normal round (0,x)")
    {
        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        // We send a prepare as we were pristine
        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 2);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0][0].counter == 0);
        REQUIRE(scp.mHeardFromQuorums[0][0].value == xValue);

        // We have a quorum including us
        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 2);

        SCPEnvelope prepared1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared3);
        REQUIRE(scp.mEnvs.size() == 2);

        scp.receiveEnvelope(prepared2);
        REQUIRE(scp.mEnvs.size() == 3);

        REQUIRE(scp.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[2].statement.pledges.type() ==
                SCPStatementType::COMMIT);

        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 3);

        SCPEnvelope commit1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMIT);
        SCPEnvelope commit2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMIT);
        SCPEnvelope commit3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMIT);

        scp.receiveEnvelope(commit2);
        REQUIRE(scp.mEnvs.size() == 3);

        scp.receiveEnvelope(commit1);
        REQUIRE(scp.mEnvs.size() == 4);

        REQUIRE(scp.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[3].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[3].statement.pledges.type() ==
                SCPStatementType::COMMITTED);

        scp.receiveEnvelope(commit3);
        REQUIRE(scp.mEnvs.size() == 5);

        REQUIRE(scp.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[4].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[4].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[4].statement.pledges.type() ==
                SCPStatementType::COMMITTED);

        SCPEnvelope committed1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);

        scp.receiveEnvelope(committed3);
        REQUIRE(scp.mEnvs.size() == 5);
        // The slot should not have externalized yet
        REQUIRE(scp.mExternalizedValues.find(0) ==
                scp.mExternalizedValues.end());

        scp.receiveEnvelope(committed1);
        REQUIRE(scp.mEnvs.size() == 5);
        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues.size() == 1);
        REQUIRE(scp.mExternalizedValues[0] == xValue);

        scp.receiveEnvelope(committed2);
        REQUIRE(scp.mEnvs.size() == 5);
        REQUIRE(scp.mExternalizedValues.size() == 1);
    }

    SECTION("x<y, prepare (0,x), prepared (0,y) by v-blocking")
    {
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        SCPEnvelope prepared1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);
        scp.receiveEnvelope(prepared2);
        REQUIRE(scp.mEnvs.size() == 4);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARE);
        REQUIRE(!scp.mEnvs[1].statement.pledges.prepare().prepared);
        REQUIRE(scp.mEnvs[1].statement.pledges.prepare().excepted.size() == 0);

        REQUIRE(scp.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[2].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[2].statement.pledges.type() ==
                SCPStatementType::PREPARED);

        REQUIRE(scp.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[3].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[3].statement.pledges.type() ==
                SCPStatementType::COMMIT);
    }

    SECTION("x<y, prepare (0,y), prepared (0,x) by v-blocking")
    {
        REQUIRE(scp.prepareValue(0, yValue));
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1);
        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 1);

        SCPEnvelope prepared1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(prepared2);
        REQUIRE(scp.mEnvs.size() == 2);

        // We're supposed to emit a PREPARED message here as the smaller ballot
        // (0,x) effectively prepared.
        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARED);
    }

    SECTION("x<y, prepare (0,x), prepare (0,y) by quorum")
    {
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 2);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARE);
        REQUIRE(!scp.mEnvs[1].statement.pledges.prepare().prepared);
        REQUIRE(scp.mEnvs[1].statement.pledges.prepare().excepted.size() == 0);

        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 3);

        REQUIRE(scp.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[2].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[2].statement.pledges.type() ==
                SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 3);
    }

    SECTION("x<y, prepare (0,y), prepare (0,x) by quorum")
    {
        REQUIRE(scp.prepareValue(0, yValue));
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1);
        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 2);

        // We're supposed to emit a PREPARED message here as the smaller ballot
        // (0,x) effectively prepared.
        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARED);
    }

    SECTION("prepare (0,x), committed (*,y) by quorum")
    {
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPEnvelope committed1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(1, yValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(2, yValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(4, yValue),
                         SCPStatementType::COMMITTED);

        scp.receiveEnvelope(committed1);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        scp.receiveEnvelope(committed3);
        REQUIRE(scp.mEnvs.size() == 2);
        // We're on different ballots
        REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues[0] == yValue);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 4);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::COMMITTED);
    }

    SECTION("x<y, prepare (0,x), committed (0,y) by quorum")
    {
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPEnvelope committed1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::COMMITTED);

        scp.receiveEnvelope(committed1);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(committed3);
        REQUIRE(scp.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues[0] == yValue);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::COMMITTED);
    }

    SECTION("x<y, prepare (0,y), committed (0,x) by quorum")
    {
        REQUIRE(scp.prepareValue(0, yValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPEnvelope committed1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);

        scp.receiveEnvelope(committed1);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(committed3);
        REQUIRE(scp.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues[0] == xValue);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 1);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::COMMITTED);
    }

    SECTION("pristine, committed (0,x) by quorum")
    {
        REQUIRE(scp.mEnvs.size() == 0);

        SCPEnvelope committed1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);
        SCPEnvelope committed3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMITTED);

        scp.receiveEnvelope(committed1);
        REQUIRE(scp.mEnvs.size() == 0);

        scp.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(scp.mEnvs.size() == 0);

        scp.receiveEnvelope(committed3);
        REQUIRE(scp.mEnvs.size() == 1);

        // The slot should have externalized the value
        REQUIRE(scp.mExternalizedValues[0] == xValue);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::COMMITTED);
    }

    SECTION("prepare (0,x), prepare (1,y), * (0,z)")
    {
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(1, yValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 2);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 1);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        SCPEnvelope prepare2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, zValue),
                         SCPStatementType::PREPARE);
        scp.receiveEnvelope(prepare2);

        SCPEnvelope commit3 =
            makeEnvelope(v3SecretKey, qSetHash, 0, SCPBallot(0, zValue),
                         SCPStatementType::COMMIT);
        scp.receiveEnvelope(commit3);

        // The envelopes should have no effect
        REQUIRE(scp.mEnvs.size() == 2);
    }

    SECTION("x<y, prepare (0,x), prepare (0,y), attempt (0,y)")
    {
        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare2 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare3 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare2);
        REQUIRE(scp.mEnvs.size() == 1);

        scp.receiveEnvelope(prepare3);
        REQUIRE(scp.mEnvs.size() == 2);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        REQUIRE(scp.prepareValue(0, yValue));
        // No effect on this round
        REQUIRE(scp.mEnvs.size() == 2);
    }

    SECTION("x<y, pledge to commit (0,x), accept cancel on (2,y)")
    {
        // 1 and 2 prepare (0,x)
        SCPEnvelope prepare1_x =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        SCPEnvelope prepare2_x =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare1_x);
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        scp.receiveEnvelope(prepare2_x);
        REQUIRE(scp.mEnvs.size() == 2);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARED);

        SCPEnvelope prepared1_x =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2_x =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1_x);
        REQUIRE(scp.mEnvs.size() == 2);

        scp.receiveEnvelope(prepared2_x);
        REQUIRE(scp.mEnvs.size() == 3);

        REQUIRE(scp.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[2].statement.pledges.type() ==
                SCPStatementType::COMMIT);

        // then our node becomes laggy for a while time during which 1, 2, 3
        // confirms prepare on (1,y) but 3 dies. Then we come back and 2
        // prepares (2,y)

        // 1 and 2 prepare (2,y)
        SCPEnvelope prepare1_y =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(2, yValue),
                         SCPStatementType::PREPARE);
        prepare1_y.statement.pledges.prepare().prepared.activate() =
            SCPBallot(1, yValue);
        signEnvelope(v1SecretKey, prepare1_y);

        SCPEnvelope prepare2_y =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(2, yValue),
                         SCPStatementType::PREPARE);
        prepare2_y.statement.pledges.prepare().prepared.activate() =
            SCPBallot(1, yValue);
        signEnvelope(v2SecretKey, prepare2_y);

        scp.receiveEnvelope(prepare1_y);
        REQUIRE(scp.mEnvs.size() == 4);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 1);

        REQUIRE(scp.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[3].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[3].statement.ballot.counter == 2);
        REQUIRE(scp.mEnvs[3].statement.pledges.type() ==
                SCPStatementType::PREPARE);
        REQUIRE(scp.mEnvs[3].statement.pledges.prepare().excepted.size() == 1);
        REQUIRE(scp.mEnvs[3].statement.pledges.prepare().excepted[0].counter ==
                0);
        REQUIRE(scp.mEnvs[3].statement.pledges.prepare().excepted[0].value ==
                xValue);
        REQUIRE(!scp.mEnvs[3].statement.pledges.prepare().prepared);

        scp.receiveEnvelope(prepare2_y);
        REQUIRE(scp.mEnvs.size() == 5);
        REQUIRE(scp.mHeardFromQuorums[0].size() == 2);
        REQUIRE(scp.mHeardFromQuorums[0][1].counter == 2);
        REQUIRE(scp.mHeardFromQuorums[0][1].value == yValue);

        REQUIRE(scp.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[4].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[4].statement.ballot.counter == 2);
        REQUIRE(scp.mEnvs[4].statement.pledges.type() ==
                SCPStatementType::PREPARED);

        SCPEnvelope prepared1_y =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(2, yValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2_y =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(2, yValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1_y);
        REQUIRE(scp.mEnvs.size() == 5);

        scp.receiveEnvelope(prepared2_y);
        REQUIRE(scp.mEnvs.size() == 6);

        REQUIRE(scp.mEnvs[5].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[5].statement.ballot.value == yValue);
        REQUIRE(scp.mEnvs[5].statement.ballot.counter == 2);
        REQUIRE(scp.mEnvs[5].statement.pledges.type() ==
                SCPStatementType::COMMIT);

        // finally things go south and we attempt x again
        REQUIRE(scp.prepareValue(0, xValue));
        REQUIRE(scp.mEnvs.size() == 7);

        // we check the prepare message is all in order
        REQUIRE(scp.mEnvs[6].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[6].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[6].statement.ballot.counter == 3);
        REQUIRE(scp.mEnvs[6].statement.pledges.type() ==
                SCPStatementType::PREPARE);
        REQUIRE(scp.mEnvs[6].statement.pledges.prepare().excepted.size() == 2);
        REQUIRE(scp.mEnvs[6].statement.pledges.prepare().excepted[0].counter ==
                0);
        REQUIRE(scp.mEnvs[6].statement.pledges.prepare().excepted[0].value ==
                xValue);
        REQUIRE(scp.mEnvs[6].statement.pledges.prepare().excepted[1].counter ==
                2);
        REQUIRE(scp.mEnvs[6].statement.pledges.prepare().excepted[1].value ==
                yValue);
        REQUIRE(scp.mEnvs[6].statement.pledges.prepare().prepared);
        REQUIRE((*scp.mEnvs[6].statement.pledges.prepare().prepared).counter ==
                0);
        REQUIRE((*scp.mEnvs[6].statement.pledges.prepare().prepared).value ==
                xValue);
    }

    SECTION("missing prepared (0,y), commit (0,y)")
    {
        SCPEnvelope prepare1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARE);
        scp.receiveEnvelope(prepare1);
        REQUIRE(scp.mEnvs.size() == 1);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        SCPEnvelope commit1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::COMMIT);

        // the node must have requested evidence
        scp.receiveEnvelope(commit1, [](SCP::EnvelopeState s)
                            {
            REQUIRE(s == SCP::EnvelopeState::STATEMENTS_MISSING);
        });
        REQUIRE(scp.mEnvs.size() == 1);
    }

    SECTION("prepared(0,y) on pristine slot should not bump")
    {
        SCPEnvelope prepared1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 0);
    }

    SECTION("commit(0,y) on pristine slot should not bump and request evidence")
    {
        SCPEnvelope commit1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::COMMIT);

        // the node must have requested evidence
        scp.receiveEnvelope(commit1, [](SCP::EnvelopeState s)
                            {
            REQUIRE(s == SCP::EnvelopeState::STATEMENTS_MISSING);
        });
        REQUIRE(scp.mEnvs.size() == 0);
    }

    SECTION("committed(0,y) on pristine slot should not bump")
    {
        SCPEnvelope commit1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, yValue),
                         SCPStatementType::COMMIT);

        scp.receiveEnvelope(commit1);
        REQUIRE(scp.mEnvs.size() == 0);
    }

    SECTION("bumpToBallot prevented once committed")
    {
        SCPEnvelope prepared1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1);
        REQUIRE(scp.mEnvs.size() == 0);

        scp.receiveEnvelope(prepared2);
        REQUIRE(scp.mEnvs.size() == 3);

        REQUIRE(scp.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[0].statement.pledges.type() ==
                SCPStatementType::PREPARE);

        REQUIRE(scp.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[1].statement.pledges.type() ==
                SCPStatementType::PREPARED);

        REQUIRE(scp.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[2].statement.pledges.type() ==
                SCPStatementType::COMMIT);

        SCPEnvelope commit1 =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMIT);
        SCPEnvelope commit2 =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(0, xValue),
                         SCPStatementType::COMMIT);

        scp.receiveEnvelope(commit1);
        REQUIRE(scp.mEnvs.size() == 3);

        scp.receiveEnvelope(commit2);
        REQUIRE(scp.mEnvs.size() == 4);

        REQUIRE(scp.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[3].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[3].statement.pledges.type() ==
                SCPStatementType::COMMITTED);

        SCPEnvelope prepared1_y =
            makeEnvelope(v1SecretKey, qSetHash, 0, SCPBallot(1, yValue),
                         SCPStatementType::PREPARED);
        SCPEnvelope prepared2_y =
            makeEnvelope(v2SecretKey, qSetHash, 0, SCPBallot(1, yValue),
                         SCPStatementType::PREPARED);

        scp.receiveEnvelope(prepared1_y);
        REQUIRE(scp.mEnvs.size() == 5);

        REQUIRE(scp.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[4].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[4].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[3].statement.pledges.type() ==
                SCPStatementType::COMMITTED);

        scp.receiveEnvelope(prepared2_y);
        REQUIRE(scp.mEnvs.size() == 6);

        REQUIRE(scp.mEnvs[5].nodeID == v0NodeID);
        REQUIRE(scp.mEnvs[5].statement.ballot.value == xValue);
        REQUIRE(scp.mEnvs[5].statement.ballot.counter == 0);
        REQUIRE(scp.mEnvs[5].statement.pledges.type() ==
                SCPStatementType::COMMITTED);
    }

    // TODO(spolu) add generic test to check that no statement emitted
    //             contradict itself
}
