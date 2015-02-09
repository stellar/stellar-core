// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"

#include "util/asio.h"

#include "lib/catch.hpp"
#include "fba/FBA.h"
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

class TestFBA : public FBA
{
  public:
    TestFBA(const SecretKey& secretKey,
            const FBAQuorumSet& qSetLocal)
        : FBA(secretKey, qSetLocal)
    {
    }
    void storeQuorumSet(FBAQuorumSet qSet)
    {
        Hash qSetHash = sha512_256(xdr::xdr_to_msg(qSet));
        mQuorumSets[qSetHash] = qSet;
    }

    void validateValue(const uint64& slotIndex,
                       const Hash& nodeID,
                       const Value& value,
                       std::function<void(bool)> const& cb)
    {
        cb(true);
    }

    void validateBallot(const uint64& slotIndex,
                        const Hash& nodeID,
                        const FBABallot& ballot,
                        std::function<void(bool)> const& cb)
    {
        cb(true);
    }

    void ballotDidPrepare(const uint64& slotIndex,
                          const FBABallot& ballot)
    {
    }
    void ballotDidPrepared(const uint64& slotIndex,
                           const FBABallot& ballot)
    {
    }
    void ballotDidCommit(const uint64& slotIndex,
                         const FBABallot& ballot)
    {
    }
    void ballotDidCommitted(const uint64& slotIndex,
                            const FBABallot& ballot)
    {
    }
    void ballotDidHearFromQuorum(const uint64& slotIndex,
                                 const FBABallot& ballot) 
    {
        mHeardFromQuorums[slotIndex].push_back(ballot);
    }

    void valueExternalized(const uint64& slotIndex,
                           const Value& value)
    {
        if (mExternalizedValues.find(slotIndex) != mExternalizedValues.end())
        {
            throw std::out_of_range("Value already externalized");
        }
        mExternalizedValues[slotIndex] = value;
    }

    void retrieveQuorumSet(const Hash& nodeID,
                           const Hash& qSetHash,
                           std::function<void(const FBAQuorumSet&)> const& cb)
    {
        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {
            CLOG(DEBUG, "FBA") << "TestFBA::retrieveQuorumSet"
                << " qSet: " << binToHex(qSetHash).substr(0,6)
                << " nodeID: " << binToHex(nodeID).substr(0,6)
                << " OK";
            return cb(mQuorumSets[qSetHash]);
        }
        else
        {
            CLOG(DEBUG, "FBA") << "TestFBA::retrieveQuorumSet"
                << " qSet: " << binToHex(qSetHash).substr(0,6)
                << " nodeID: " << binToHex(nodeID).substr(0,6)
                << " FAIL";
        }
    }
    void emitEnvelope(const FBAEnvelope& envelope)
    {
        mEnvs.push_back(envelope);
    }

    std::map<Hash, FBAQuorumSet>              mQuorumSets;
    std::vector<FBAEnvelope>                  mEnvs;
    std::map<uint64, Value>                   mExternalizedValues;
    std::map<uint64, std::vector<FBABallot>>  mHeardFromQuorums;
};


static FBAEnvelope 
makeEnvelope(const SecretKey& secretKey,
             const Hash& qSetHash,
             const uint64& slotIndex,
             const FBABallot& ballot,
             const FBAStatementType& type)
{
    FBAEnvelope envelope;

    envelope.nodeID = secretKey.getPublicKey();
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.ballot = ballot;
    envelope.statement.quorumSetHash = qSetHash;
    envelope.statement.pledges.type(type);
    envelope.signature = 
        secretKey.sign(xdr::xdr_to_msg(envelope.statement));

    return envelope;
}

void
signEnvelope(const SecretKey& secretKey,
             FBAEnvelope& envelope)
{
    envelope.signature = 
        secretKey.sign(xdr::xdr_to_msg(envelope.statement));
}

#define CREATE_VALUE(X) \
    const Hash X##ValueHash = sha512_256("SEED_VALUE_HASH_" #X); \
    const Value X##Value = xdr::xdr_to_opaque(X##ValueHash);

TEST_CASE("protocol core4", "[fba]")
{
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    FBAQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));

    TestFBA fba(v0SecretKey, qSet);

    fba.storeQuorumSet(qSet);

    CREATE_VALUE(x);
    CREATE_VALUE(y);
    CREATE_VALUE(z);

    REQUIRE(xValue < yValue);

    CLOG(INFO, "FBA") << "";
    CLOG(INFO, "FBA") << "BEGIN TEST";

    SECTION("prepareValue x")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.slotIndex == 0);
        REQUIRE(fba.mEnvs[0].statement.quorumSetHash == qSetHash);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(!fba.mEnvs[0].statement.pledges.prepare().prepared);
        REQUIRE(fba.mEnvs[0].statement.pledges.prepare().excepted.size() == 0);
    }

    SECTION("normal round (0,x)")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 1);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 0);

        // We send a prepare as we were pristine
        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 2);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 1);
        REQUIRE(fba.mHeardFromQuorums[0][0].counter == 0);
        REQUIRE(fba.mHeardFromQuorums[0][0].value == xValue);

        // We have a quorum including us
        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepare3);
        REQUIRE(fba.mEnvs.size() == 2);

        FBAEnvelope prepared1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared3);
        REQUIRE(fba.mEnvs.size() == 2);

        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].statement.pledges.type() == FBAStatementType::COMMIT);
        
        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 3);

        FBAEnvelope commit1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);

        fba.receiveEnvelope(commit2);
        REQUIRE(fba.mEnvs.size() == 3);

        fba.receiveEnvelope(commit1);
        REQUIRE(fba.mEnvs.size() == 4);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[3].statement.pledges.type() == FBAStatementType::COMMITTED);

        fba.receiveEnvelope(commit3);
        REQUIRE(fba.mEnvs.size() == 5);

        REQUIRE(fba.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[4].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[4].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[4].statement.pledges.type() == FBAStatementType::COMMITTED);

        FBAEnvelope committed1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed3);
        REQUIRE(fba.mEnvs.size() == 5);
        // The slot should not have externalized yet
        REQUIRE(fba.mExternalizedValues.find(0) == 
            fba.mExternalizedValues.end());

        fba.receiveEnvelope(committed1);
        REQUIRE(fba.mEnvs.size() == 5);
        // The slot should have externalized the value
        REQUIRE(fba.mExternalizedValues.size() == 1);
        REQUIRE(fba.mExternalizedValues[0] == xValue);

        fba.receiveEnvelope(committed2);
        REQUIRE(fba.mEnvs.size() == 5);
        REQUIRE(fba.mExternalizedValues.size() == 1);
    }

    SECTION("x<y, prepare (0,x), prepared (0,y) by v-blocking")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepared1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                             FBABallot(0, yValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                             FBABallot(0, yValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 1);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 0);
        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 4);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 1);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(!fba.mEnvs[1].statement.pledges.prepare().prepared);
        REQUIRE(fba.mEnvs[1].statement.pledges.prepare().excepted.size() == 0);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].statement.pledges.type() == FBAStatementType::PREPARED);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[3].statement.pledges.type() == FBAStatementType::COMMIT);
    }

    SECTION("x<y, prepare (0,y), prepared (0,x) by v-blocking")
    {
        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope prepared1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 2);

        // We're supposed to emit a PREPARED message here as the smaller ballot
        // (0,x) effectively prepared.
        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARED);
    }
    
    SECTION("x<y, prepare (0,x), prepare (0,y) by quorum")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 2);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(!fba.mEnvs[1].statement.pledges.prepare().prepared);
        REQUIRE(fba.mEnvs[1].statement.pledges.prepare().excepted.size() == 0);

        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].statement.pledges.type() == FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepare3);
        REQUIRE(fba.mEnvs.size() == 3);
    }

    SECTION("x<y, prepare (0,y), prepare (0,x) by quorum")
    {
        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(prepare3);
        REQUIRE(fba.mEnvs.size() == 2);

        // We're supposed to emit a PREPARED message here as the smaller ballot
        // (0,x) effectively prepared.
        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARED);
    }

    SECTION("prepare (0,x), committed (*,y) by quorum")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(1, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(2, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                              FBABallot(4, yValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(fba.mEnvs.size() == 1);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 0);

        fba.receiveEnvelope(committed3);
        REQUIRE(fba.mEnvs.size() == 2);
        // We're on different ballots
        REQUIRE(fba.mHeardFromQuorums[0].size() == 0);

        // The slot should have externalized the value
        REQUIRE(fba.mExternalizedValues[0] == yValue);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 4);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("x<y, prepare (0,x), committed (0,y) by quorum")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(committed3);
        REQUIRE(fba.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(fba.mExternalizedValues[0] == yValue);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("x<y, prepare (0,y), committed (0,x) by quorum")
    {
        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(committed3);
        REQUIRE(fba.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(fba.mExternalizedValues[0] == xValue);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 1);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("pristine, committed (0,x) by quorum")
    {
        REQUIRE(fba.mEnvs.size() == 0);

        FBAEnvelope committed1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(fba.mEnvs.size() == 0);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(fba.mEnvs.size() == 0);

        fba.receiveEnvelope(committed3);
        REQUIRE(fba.mEnvs.size() == 1);

        // The slot should have externalized the value
        REQUIRE(fba.mExternalizedValues[0] == xValue);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("prepare (0,x), prepare (1,y), * (0,z)")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(1, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 2);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 1);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                            FBABallot(0, zValue),
                                            FBAStatementType::PREPARE);
        fba.receiveEnvelope(prepare2);

        FBAEnvelope commit3 = makeEnvelope(v3SecretKey, qSetHash, 0,
                                           FBABallot(0, zValue),
                                           FBAStatementType::COMMIT);
        fba.receiveEnvelope(commit3);

        // The envelopes should have no effect
        REQUIRE(fba.mEnvs.size() == 2);
    }

    SECTION("x<y, prepare (0,x), prepare (0,y), attempt (0,y)")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(prepare3);
        REQUIRE(fba.mEnvs.size() == 2);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARE);

        REQUIRE(fba.prepareValue(0, yValue));
        // No effect on this round
        REQUIRE(fba.mEnvs.size() == 2);
    }

    SECTION("x<y, pledge to commit (0,x), accept cancel on (2,y)")
    {
        // 1 and 2 prepare (0,x)
        FBAEnvelope prepare1_x = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2_x = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1_x);
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2_x);
        REQUIRE(fba.mEnvs.size() == 2);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 1);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_x = makeEnvelope(v1SecretKey, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_x = makeEnvelope(v2SecretKey, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_x);
        REQUIRE(fba.mEnvs.size() == 2);

        fba.receiveEnvelope(prepared2_x);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].statement.pledges.type() == FBAStatementType::COMMIT);

        // then our node becomes laggy for a while time during which 1, 2, 3
        // confirms prepare on (1,y) but 3 dies. Then we come back and 2
        // prepares (2,y)

        // 1 and 2 prepare (2,y)
        FBAEnvelope prepare1_y = makeEnvelope(v1SecretKey, qSetHash, 0,
                                              FBABallot(2, yValue),
                                              FBAStatementType::PREPARE);
        prepare1_y.statement.pledges.prepare().prepared.activate() = 
            FBABallot(1, yValue);
        signEnvelope(v1SecretKey, prepare1_y);

        FBAEnvelope prepare2_y = makeEnvelope(v2SecretKey, qSetHash, 0,
                                              FBABallot(2, yValue),
                                              FBAStatementType::PREPARE);
        prepare2_y.statement.pledges.prepare().prepared.activate() = 
            FBABallot(1, yValue);
        signEnvelope(v2SecretKey, prepare2_y);

        fba.receiveEnvelope(prepare1_y);
        REQUIRE(fba.mEnvs.size() == 4);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 1);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[3].statement.ballot.counter == 2);
        REQUIRE(fba.mEnvs[3].statement.pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(fba.mEnvs[3].statement.pledges.prepare().excepted.size() == 1);
        REQUIRE(fba.mEnvs[3].statement.pledges.prepare().excepted[0].counter == 0);
        REQUIRE(fba.mEnvs[3].statement.pledges.prepare().excepted[0].value == xValue);
        REQUIRE(!fba.mEnvs[3].statement.pledges.prepare().prepared);

        fba.receiveEnvelope(prepare2_y);
        REQUIRE(fba.mEnvs.size() == 5);
        REQUIRE(fba.mHeardFromQuorums[0].size() == 2);
        REQUIRE(fba.mHeardFromQuorums[0][1].counter == 2);
        REQUIRE(fba.mHeardFromQuorums[0][1].value == yValue);

        REQUIRE(fba.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[4].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[4].statement.ballot.counter == 2);
        REQUIRE(fba.mEnvs[4].statement.pledges.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_y = makeEnvelope(v1SecretKey, qSetHash, 0,
                                               FBABallot(2, yValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_y = makeEnvelope(v2SecretKey, qSetHash, 0,
                                               FBABallot(2, yValue),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_y);
        REQUIRE(fba.mEnvs.size() == 5);

        fba.receiveEnvelope(prepared2_y);
        REQUIRE(fba.mEnvs.size() == 6);

        REQUIRE(fba.mEnvs[5].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[5].statement.ballot.value == yValue);
        REQUIRE(fba.mEnvs[5].statement.ballot.counter == 2);
        REQUIRE(fba.mEnvs[5].statement.pledges.type() == FBAStatementType::COMMIT);

        // finally things go south and we attempt x again
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 7);

        // we check the prepare message is all in order
        REQUIRE(fba.mEnvs[6].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[6].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[6].statement.ballot.counter == 3);
        REQUIRE(fba.mEnvs[6].statement.pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(fba.mEnvs[6].statement.pledges.prepare().excepted.size() == 2);
        REQUIRE(fba.mEnvs[6].statement.pledges.prepare().excepted[0].counter == 0);
        REQUIRE(fba.mEnvs[6].statement.pledges.prepare().excepted[0].value == xValue);
        REQUIRE(fba.mEnvs[6].statement.pledges.prepare().excepted[1].counter == 2);
        REQUIRE(fba.mEnvs[6].statement.pledges.prepare().excepted[1].value == yValue);
        REQUIRE(fba.mEnvs[6].statement.pledges.prepare().prepared);
        REQUIRE((*fba.mEnvs[6].statement.pledges.prepare().prepared).counter == 0);
        REQUIRE((*fba.mEnvs[6].statement.pledges.prepare().prepared).value == xValue);
    }

    SECTION("missing prepared (0,y), commit (0,y)")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope commit1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                           FBABallot(0, yValue),
                                           FBAStatementType::COMMIT);

        // the node must have requested evidence
        fba.receiveEnvelope(
            commit1, 
            [] (FBA::EnvelopeState s)
            {
                REQUIRE(s == FBA::EnvelopeState::STATEMENTS_MISSING);
            });
        REQUIRE(fba.mEnvs.size() == 1);
    }

    SECTION("prepared(0,y) on pristine slot should not bump")
    {
        FBAEnvelope prepared1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 0);
    }

    SECTION("commit(0,y) on pristine slot should not bump and request evidence")
    {
        FBAEnvelope commit1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                           FBABallot(0, yValue),
                                           FBAStatementType::COMMIT);

        // the node must have requested evidence
        fba.receiveEnvelope(
            commit1, 
            [] (FBA::EnvelopeState s)
            {
                REQUIRE(s == FBA::EnvelopeState::STATEMENTS_MISSING);
            });
        REQUIRE(fba.mEnvs.size() == 0);
    }

    SECTION("committed(0,y) on pristine slot should not bump")
    {
        FBAEnvelope commit1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                           FBABallot(0, yValue),
                                           FBAStatementType::COMMIT);

        fba.receiveEnvelope(commit1);
        REQUIRE(fba.mEnvs.size() == 0);
    }

    SECTION("bumpToBallot prevented once committed")
    {
        FBAEnvelope prepared1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 0);

        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].statement.pledges.type() == FBAStatementType::PREPARE);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].statement.pledges.type() == FBAStatementType::PREPARED);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].statement.pledges.type() == FBAStatementType::COMMIT);

        FBAEnvelope commit1 = makeEnvelope(v1SecretKey, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit2 = makeEnvelope(v2SecretKey, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);

        fba.receiveEnvelope(commit1);
        REQUIRE(fba.mEnvs.size() == 3);

        fba.receiveEnvelope(commit2);
        REQUIRE(fba.mEnvs.size() == 4);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[3].statement.pledges.type() == FBAStatementType::COMMITTED);


        FBAEnvelope prepared1_y = makeEnvelope(v1SecretKey, qSetHash, 0,
                                               FBABallot(1, yValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_y = makeEnvelope(v2SecretKey, qSetHash, 0,
                                               FBABallot(1, yValue),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_y);
        REQUIRE(fba.mEnvs.size() == 5);

        REQUIRE(fba.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[4].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[4].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[3].statement.pledges.type() == FBAStatementType::COMMITTED);

        fba.receiveEnvelope(prepared2_y);
        REQUIRE(fba.mEnvs.size() == 6);

        REQUIRE(fba.mEnvs[5].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[5].statement.ballot.value == xValue);
        REQUIRE(fba.mEnvs[5].statement.ballot.counter == 0);
        REQUIRE(fba.mEnvs[5].statement.pledges.type() == FBAStatementType::COMMITTED);
    }

    // TODO(spolu) add generic test to check that no statement emitted 
    //             contradict itself
}

