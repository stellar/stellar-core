// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "lib/catch.hpp"
#include "fba/FBA.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"

using namespace stellar;

using xdr::operator<;
using xdr::operator==;

class TestFBA : public FBA
{
  public:
    TestFBA(const uint256& validationSeed,
            const FBAQuorumSet& qSetLocal)
        : FBA(validationSeed, qSetLocal)
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
    void ballotDidAbort(const uint64& slotIndex,
                        const FBABallot& ballot)
    {
        mAbortedBallots[slotIndex].push_back(ballot);
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
            LOG(INFO) << "FBA::Client::retrieveQuorumSet"
                      << " " << binToHex(qSetHash).substr(0,6)
                      << "@" << binToHex(nodeID).substr(0,6)
                      << " OK";
            return cb(mQuorumSets[qSetHash]);
        }
        else
        {
            LOG(INFO) << "FBA::Client::retrieveQuorumSet"
                      << " " << binToHex(qSetHash).substr(0,6)
                      << "@" << binToHex(nodeID).substr(0,6)
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
    std::map<uint64, std::vector<FBABallot>>  mAbortedBallots;
};


static FBAEnvelope 
makeStEnvelope(const uint256& nodeID,
               const Hash& qSetHash,
               const uint64& slotIndex,
               const FBABallot& ballot,
               const FBAStatementType& type)
{
    FBAEnvelope envelope;

    envelope.nodeID = nodeID;
    envelope.slotIndex = slotIndex;
    envelope.payload.type(FBAEnvelopeType::STATEMENT);
    envelope.payload.statement().ballot = ballot;
    envelope.payload.statement().quorumSetHash = qSetHash;
    envelope.payload.statement().pledges.type(type);

    return envelope;
}

FBAEnvelope
makeVPEnvelope(const uint256& nodeID,
               const Hash& qSetHash,
               const uint64& slotIndex,
               const Value& value)
{
    FBAEnvelope envelope;

    envelope.nodeID = nodeID;
    envelope.slotIndex = slotIndex;
    envelope.payload.type(FBAEnvelopeType::VALUE_PART);
    envelope.payload.part().value = value;
    envelope.payload.part().quorumSetHash = qSetHash;

    return envelope;
}

#define CREATE_NODE(N) \
    const Hash v##N##VSeed = sha512_256("SEED_VALIDATION_SEED_" #N); \
    const Hash v##N##NodeID = makePublicKey(v##N##VSeed);

#define CREATE_VALUE(X) \
    const Hash X##ValueHash = sha512_256("SEED_VALUE_HASH_" #X); \
    const Value X##Value = xdr::xdr_to_opaque(X##ValueHash);

TEST_CASE("protocol core4", "[fba]")
{
    CREATE_NODE(0);
    CREATE_NODE(1);
    CREATE_NODE(2);
    CREATE_NODE(3);

    FBAQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));

    TestFBA fba(v0VSeed, qSet);

    fba.storeQuorumSet(qSet);

    CREATE_VALUE(x);
    CREATE_VALUE(y);
    CREATE_VALUE(z);

    REQUIRE(xValue < yValue);

    LOG(INFO) << "<<<< BEGIN FBA TEST >>>>";

    SECTION("prepareValue x")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].slotIndex == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().quorumSetHash == qSetHash);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(!fba.mEnvs[0].payload.statement().pledges.prepare().prepared);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.prepare().excepted.size() == 0);
    }

    SECTION("normal round (0,x)")
    {
        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeStEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 1);

        // We send a prepare as we were pristine
        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 2);

        // We have a quorum including us
        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepare3);
        REQUIRE(fba.mEnvs.size() == 2);

        FBAEnvelope prepared1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared3 = makeStEnvelope(v3NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared3);
        REQUIRE(fba.mEnvs.size() == 2);

        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].payload.statement().pledges.type() == FBAStatementType::COMMIT);
        
        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 3);

        FBAEnvelope commit1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::COMMIT);
        FBAEnvelope commit2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::COMMIT);
        FBAEnvelope commit3 = makeStEnvelope(v3NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::COMMIT);

        fba.receiveEnvelope(commit2);
        REQUIRE(fba.mEnvs.size() == 3);

        fba.receiveEnvelope(commit1);
        REQUIRE(fba.mEnvs.size() == 4);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[3].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[3].payload.statement().pledges.type() == FBAStatementType::COMMITTED);

        fba.receiveEnvelope(commit3);
        REQUIRE(fba.mEnvs.size() == 5);

        REQUIRE(fba.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[4].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[4].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[4].payload.statement().pledges.type() == FBAStatementType::COMMITTED);

        FBAEnvelope committed1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeStEnvelope(v3NodeID, qSetHash, 0,
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
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope prepared1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(0, yValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(0, yValue),
                                               FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 1);
        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 4);


        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(!fba.mEnvs[1].payload.statement().pledges.prepare().prepared);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.prepare().excepted.size() == 0);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].payload.statement().pledges.type() == FBAStatementType::PREPARED);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[3].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[3].payload.statement().pledges.type() == FBAStatementType::COMMIT);
    }

    SECTION("x<y, prepare (0,y), prepared (0,x) by v-blocking")
    {
        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope prepared1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(prepared2);
        REQUIRE(fba.mEnvs.size() == 2);

        // We're supposed to emit a PREPARED message here as the smaller ballot
        // (0,x) effectively prepared.
        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARED);
    }
    
    SECTION("x<y, prepare (0,x), prepare (0,y) by quorum")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeStEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);
        fba.receiveEnvelope(prepare3);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(!fba.mEnvs[1].payload.statement().pledges.prepare().prepared);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.prepare().excepted.size() == 0);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].payload.statement().pledges.type() == FBAStatementType::PREPARED);
    }

    SECTION("x<y, prepare (0,y), prepare (0,x) by quorum")
    {
        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeStEnvelope(v3NodeID, qSetHash, 0,
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
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARED);
    }

    SECTION("prepare (0,x), committed (*,y) by quorum")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(1, yValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                FBABallot(2, yValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeStEnvelope(v3NodeID, qSetHash, 0,
                                                FBABallot(4, yValue),
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
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 4);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("x<y, prepare (0,x), committed (0,y) by quorum")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, yValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                FBABallot(0, yValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeStEnvelope(v3NodeID, qSetHash, 0,
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
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("x<y, prepare (0,y), committed (0,x) by quorum")
    {
        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeStEnvelope(v3NodeID, qSetHash, 0,
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
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 1);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::COMMITTED);
    }

    SECTION("prepare (0,x), prepare (1,y), * (0,z)")
    {
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 1);

        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(1, yValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 2);
        REQUIRE(fba.mAbortedBallots[0][0].counter == 0);
        REQUIRE(fba.mAbortedBallots[0][0].value == xValue);

        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, zValue),
                                              FBAStatementType::PREPARE);
        fba.receiveEnvelope(prepare2);

        FBAEnvelope commit3 = makeStEnvelope(v3NodeID, qSetHash, 0,
                                             FBABallot(0, zValue),
                                             FBAStatementType::COMMIT);
        fba.receiveEnvelope(commit3);

        // The envelopes should have no effect
        REQUIRE(fba.mEnvs.size() == 2);
    }

    SECTION("x<y, prepare (0,x), prepare (0,y), attempt (0,y)")
    {
        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(prepare3);
        // No effect on this round
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.prepareValue(0, yValue));
        REQUIRE(fba.mEnvs.size() == 2);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARE);
    }

    SECTION("pledge to commit (0,x), accept cancel on (1,y)")
    {
        // 1 and 2 prepare (0,x)
        FBAEnvelope prepare1_x = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::PREPARE);
        FBAEnvelope prepare2_x = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::PREPARE);

        // 3 prepares (0,y)
        FBAEnvelope prepare3_y = makeStEnvelope(v3NodeID, qSetHash, 0,
                                                FBABallot(0, yValue),
                                                FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1_x);
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare3_y);
        REQUIRE(fba.mEnvs.size() == 1);

        fba.receiveEnvelope(prepare2_x);
        REQUIRE(fba.mEnvs.size() == 2);

        REQUIRE(fba.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[1].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[1].payload.statement().pledges.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_x = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                 FBABallot(0, xValue),
                                                 FBAStatementType::PREPARED);
        FBAEnvelope prepared2_x = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                 FBABallot(0, xValue),
                                                 FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_x);
        REQUIRE(fba.mEnvs.size() == 2);

        fba.receiveEnvelope(prepared2_x);
        REQUIRE(fba.mEnvs.size() == 3);

        REQUIRE(fba.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[2].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[2].payload.statement().pledges.type() == FBAStatementType::COMMIT);

        // then our node becomes laggy for a while time during which 1, 2, 3
        // confirms prepare on (1,y) but 3 dies. Then we come back and 2
        // prepares (2,y)

        // 1 and 2 prepare (2,y)
        FBAEnvelope prepare1_y = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(2, yValue),
                                                FBAStatementType::PREPARE);
        prepare1_y.payload.statement().pledges.prepare().prepared.activate() = 
            FBABallot(1, yValue);

        FBAEnvelope prepare2_y = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                FBABallot(2, yValue),
                                                FBAStatementType::PREPARE);
        prepare2_y.payload.statement().pledges.prepare().prepared.activate() = 
            FBABallot(1, yValue);

        fba.receiveEnvelope(prepare1_y);
        REQUIRE(fba.mEnvs.size() == 4);
        REQUIRE(fba.mAbortedBallots[0][0].counter == 0);
        REQUIRE(fba.mAbortedBallots[0][0].value == xValue);

        REQUIRE(fba.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[3].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[3].payload.statement().ballot.counter == 2);
        REQUIRE(fba.mEnvs[3].payload.statement().pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(fba.mEnvs[3].payload.statement().pledges.prepare().excepted.size() == 1);
        REQUIRE(fba.mEnvs[3].payload.statement().pledges.prepare().excepted[0].counter == 0);
        REQUIRE(fba.mEnvs[3].payload.statement().pledges.prepare().excepted[0].value == xValue);
        REQUIRE(!fba.mEnvs[3].payload.statement().pledges.prepare().prepared);

        fba.receiveEnvelope(prepare2_y);
        REQUIRE(fba.mEnvs.size() == 5);

        REQUIRE(fba.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[4].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[4].payload.statement().ballot.counter == 2);
        REQUIRE(fba.mEnvs[4].payload.statement().pledges.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_y = makeStEnvelope(v1NodeID, qSetHash, 0,
                                                 FBABallot(2, yValue),
                                                 FBAStatementType::PREPARED);
        FBAEnvelope prepared2_y = makeStEnvelope(v2NodeID, qSetHash, 0,
                                                 FBABallot(2, yValue),
                                                 FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_y);
        REQUIRE(fba.mEnvs.size() == 5);

        fba.receiveEnvelope(prepared2_y);
        REQUIRE(fba.mEnvs.size() == 6);

        REQUIRE(fba.mEnvs[5].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[5].payload.statement().ballot.value == yValue);
        REQUIRE(fba.mEnvs[5].payload.statement().ballot.counter == 2);
        REQUIRE(fba.mEnvs[5].payload.statement().pledges.type() == FBAStatementType::COMMIT);

        // finally things go south and we attempt x again
        REQUIRE(fba.prepareValue(0, xValue));
        REQUIRE(fba.mEnvs.size() == 7);

        // we check the prepare message is all in order
        REQUIRE(fba.mEnvs[6].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[6].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[6].payload.statement().ballot.counter == 3);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.type() == FBAStatementType::PREPARE);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.prepare().excepted.size() == 2);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.prepare().excepted[0].counter == 0);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.prepare().excepted[0].value == xValue);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.prepare().excepted[1].counter == 2);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.prepare().excepted[1].value == yValue);
        REQUIRE(fba.mEnvs[6].payload.statement().pledges.prepare().prepared);
        REQUIRE((*fba.mEnvs[6].payload.statement().pledges.prepare().prepared).counter == 0);
        REQUIRE((*fba.mEnvs[6].payload.statement().pledges.prepare().prepared).value == xValue);
    }

    SECTION("missing prepare (0,y), prepared (0,y), retransmission hint")
    {
        FBAEnvelope prepare1 = makeStEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeStEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);

        // never received prepare1
        fba.receiveEnvelope(prepare2);
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.value == xValue);
        REQUIRE(fba.mEnvs[0].payload.statement().ballot.counter == 0);
        REQUIRE(fba.mEnvs[0].payload.statement().pledges.type() == FBAStatementType::PREPARE);

        FBAEnvelope commit1 = makeStEnvelope(v1NodeID, qSetHash, 0,
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

    SECTION("x<y, part (0,x), receive part (0,x/y)")
    {
        bool ready = false;
        REQUIRE(fba.readyValue(
              0, 
              xValue,
              [yValue, &ready] (Value x, FBAReadyEvidence e)
              {
                  ready = true;
                  REQUIRE(x == yValue);
                  // TODO(spolu) test evidence
              }));
        REQUIRE(fba.mEnvs.size() == 1);

        REQUIRE(fba.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(fba.mEnvs[0].slotIndex == 0);
        REQUIRE(fba.mEnvs[0].payload.part().quorumSetHash == qSetHash);
        REQUIRE(fba.mEnvs[0].payload.part().value == xValue);

        FBAEnvelope part1 = makeVPEnvelope(v1NodeID, qSetHash, 0, yValue);
        FBAEnvelope part2 = makeVPEnvelope(v2NodeID, qSetHash, 0, xValue);

        fba.receiveEnvelope(part1);
        REQUIRE(fba.mEnvs.size() == 1);
        REQUIRE(!ready);

        fba.receiveEnvelope(part2);
        REQUIRE(fba.mEnvs.size() == 1);
        // TODO(spolu) update after ready callback is called
        /*
        REQUIRE(ready);
        */
    }


    // TODO(spolu) add generic test to check that no statement emitted 
    //             contradict itself
}




