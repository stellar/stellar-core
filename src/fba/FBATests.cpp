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

class TestFBAClient : public FBA::Client
{
  public:
    void setFBA(FBA* FBA)
    {
        mFBA = FBA;
    }
    void addQuorumSet(FBAQuorumSet qSet)
    {
        Hash qSetHash = sha512_256(xdr::xdr_to_msg(qSet));
        mQuorumSets[qSetHash] = qSet;
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
    void ballotDidCommit(const uint64& slotIndex,
                         const FBABallot& ballot)
    {
    }

    void valueCancelled(const uint64& slotIndex,
                        const Value& value)
    {
        mCancelledValues[slotIndex].push_back(value);
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
                           const Hash& qSetHash)
    {
        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {
            LOG(INFO) << "FBA::Client::retrieveQuorumSet"
                      << " " << binToHex(qSetHash).substr(0,6)
                      << "@" << binToHex(nodeID).substr(0,6)
                      << " OK";
            mFBA->receiveQuorumSet(nodeID, mQuorumSets[qSetHash]);
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

    void retransmissionHinted(const uint64& slotIndex,
                              const uint256& nodeID)
    {
        mRetransmissionHints[slotIndex].push_back(nodeID);
    }

    std::map<Hash, FBAQuorumSet>           mQuorumSets;
    std::vector<FBAEnvelope>               mEnvs;
    std::map<uint64, Value>                mExternalizedValues;
    std::map<uint64, std::vector<Value>>   mCancelledValues;
    std::map<uint64, std::vector<uint256>> mRetransmissionHints;

    FBA*                                   mFBA;
};


FBAEnvelope 
makeEnvelope(const uint256& nodeID,
             const Hash& qSetHash,
             const uint64& slotIndex,
             const FBABallot& ballot,
             const FBAStatementType& type)
{
    FBAEnvelope envelope;

    envelope.nodeID = nodeID;
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.ballot = ballot;
    envelope.statement.quorumSetHash = qSetHash;
    envelope.statement.body.type(type);

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

    TestFBAClient C;
    FBA fba(v0VSeed, qSet, &C);

    C.setFBA(&fba);
    C.addQuorumSet(qSet);

    CREATE_VALUE(x);
    CREATE_VALUE(y);
    CREATE_VALUE(z);

    REQUIRE(xValue < yValue);

    LOG(INFO) << "<<<< BEGIN FBA TEST >>>>";

    SECTION("attemptValue x")
    {
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(!C.mEnvs[0].statement.body.prepare().prepared);
        REQUIRE(C.mEnvs[0].statement.body.prepare().excepted.size() == 0);
    }

    SECTION("normal round (0,x)")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(C.mEnvs.size() == 1);

        // We send a prepare as we were pristine
        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 2);

        // We have a quorum including us
        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepare3);
        REQUIRE(C.mEnvs.size() == 2);

        FBAEnvelope prepared1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared3);
        REQUIRE(C.mEnvs.size() == 2);

        fba.receiveEnvelope(prepared2);
        REQUIRE(C.mEnvs.size() == 3);

        REQUIRE(C.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[2].statement.body.type() == FBAStatementType::COMMIT);
        
        fba.receiveEnvelope(prepared1);
        REQUIRE(C.mEnvs.size() == 3);

        FBAEnvelope commit1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                           FBABallot(0, xValue),
                                           FBAStatementType::COMMIT);

        fba.receiveEnvelope(commit2);
        REQUIRE(C.mEnvs.size() == 3);

        fba.receiveEnvelope(commit1);
        REQUIRE(C.mEnvs.size() == 4);

        REQUIRE(C.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[3].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[3].statement.body.type() == FBAStatementType::COMMITTED);

        fba.receiveEnvelope(commit3);
        REQUIRE(C.mEnvs.size() == 5);

        REQUIRE(C.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[4].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[4].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[4].statement.body.type() == FBAStatementType::COMMITTED);

        FBAEnvelope committed1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed3);
        REQUIRE(C.mEnvs.size() == 5);
        // The slot should not have externalized yet
        REQUIRE(C.mExternalizedValues.find(0) == 
            C.mExternalizedValues.end());

        fba.receiveEnvelope(committed1);
        REQUIRE(C.mEnvs.size() == 5);
        // The slot should have externalized the value
        REQUIRE(C.mExternalizedValues.size() == 1);
        REQUIRE(C.mExternalizedValues[0] == xValue);

        fba.receiveEnvelope(committed2);
        REQUIRE(C.mEnvs.size() == 5);
        REQUIRE(C.mExternalizedValues.size() == 1);
    }

    SECTION("x<y, attempt (0,x), prepared (0,y) by v-blocking")
    {
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 1);

        FBAEnvelope prepared1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, yValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                             FBABallot(0, yValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(C.mEnvs.size() == 1);
        fba.receiveEnvelope(prepared2);
        REQUIRE(C.mEnvs.size() == 4);


        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(!C.mEnvs[1].statement.body.prepare().prepared);
        REQUIRE(C.mEnvs[1].statement.body.prepare().excepted.size() == 0);

        REQUIRE(C.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[2].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[2].statement.body.type() == FBAStatementType::PREPARED);

        REQUIRE(C.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[3].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[3].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[3].statement.body.type() == FBAStatementType::COMMIT);
    }

    SECTION("x<y, attempt (0,y), prepared (0,x) by v-blocking")
    {
        REQUIRE(fba.attemptValue(0, yValue));
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 1);

        FBAEnvelope prepared1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                             FBABallot(0, xValue),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(C.mEnvs.size() == 1);
        fba.receiveEnvelope(prepared2);
        REQUIRE(C.mEnvs.size() == 1);

        // TODO(spolu): We don't emit a PREPARED here as our mBallot is higher,
        // but we might want to do so for later PREPARE messages (TBD with dm)
    }
    
    SECTION("x<y, attempt (0,x), prepare (0,y) by quorum")
    {
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 1);
        fba.receiveEnvelope(prepare3);
        REQUIRE(C.mEnvs.size() == 3);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(!C.mEnvs[1].statement.body.prepare().prepared);
        REQUIRE(C.mEnvs[1].statement.body.prepare().excepted.size() == 0);

        REQUIRE(C.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[2].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[2].statement.body.type() == FBAStatementType::PREPARED);
    }

    SECTION("x<y, attempt (0,y), prepare (0,x) by quorum")
    {
        REQUIRE(fba.attemptValue(0, yValue));
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 1);
        fba.receiveEnvelope(prepare3);
        REQUIRE(C.mEnvs.size() == 1);

        // TODO(spolu): We don't emit a PREPARED here as our mBallot is higher,
        // but we might want to do so for later PREPARE messages (TBD with dm)
    }

    SECTION("attempt (0,x), committed (*,y) by quorum")
    {
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(1, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(2, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(4, yValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(committed3);
        REQUIRE(C.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(C.mExternalizedValues[0] == yValue);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 4);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::COMMITTED);
    }

    SECTION("x<y, attempt (0,x), committed (0,y) by quorum")
    {
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(committed3);
        REQUIRE(C.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(C.mExternalizedValues[0] == yValue);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::COMMITTED);
    }

    SECTION("x<y, attempt (0,y), committed (0,x) by quorum")
    {
        REQUIRE(fba.attemptValue(0, yValue));
        REQUIRE(C.mEnvs.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(committed2);
        // No change yet as a v-blocking set is not enough here
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(committed3);
        REQUIRE(C.mEnvs.size() == 2);

        // The slot should have externalized the value
        REQUIRE(C.mExternalizedValues[0] == xValue);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 1);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::COMMITTED);
    }

    SECTION("attempt (0,x), prepare (1,y), * (0,z)")
    {
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 1);

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(1, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(C.mEnvs.size() == 2);
        REQUIRE(C.mCancelledValues[0][0] == xValue);

        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, zValue),
                                            FBAStatementType::PREPARE);
        fba.receiveEnvelope(prepare2);

        FBAEnvelope commit3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                           FBABallot(0, zValue),
                                           FBAStatementType::COMMIT);
        fba.receiveEnvelope(commit3);

        // The envelopes should have no effect
        REQUIRE(C.mEnvs.size() == 2);
    }

    SECTION("x<y, prepare (0,x), prepare (0,y), attempt (0,y)")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, yValue),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(prepare3);
        // No effect on this round
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(fba.attemptValue(0, yValue));
        REQUIRE(C.mEnvs.size() == 2);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::PREPARE);
    }

    SECTION("pledge to commit (0,x), accept cancel on (1,y)")
    {
        // 1 and 2 prepare (0,x)
        FBAEnvelope prepare1_x = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2_x = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValue),
                                              FBAStatementType::PREPARE);

        // 3 prepares (0,y)
        FBAEnvelope prepare3_y = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, yValue),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1_x);
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare3_y);
        REQUIRE(C.mEnvs.size() == 1);

        fba.receiveEnvelope(prepare2_x);
        REQUIRE(C.mEnvs.size() == 2);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_x = makeEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_x = makeEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(0, xValue),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_x);
        REQUIRE(C.mEnvs.size() == 2);

        fba.receiveEnvelope(prepared2_x);
        REQUIRE(C.mEnvs.size() == 3);

        REQUIRE(C.mEnvs[2].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[2].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[2].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[2].statement.body.type() == FBAStatementType::COMMIT);

        // then our node becomes laggy for a while time during which 1, 2, 3
        // confirms prepare on (1,y) but 3 dies. Then we come back and 2
        // prepares (2,y)

        // 1 and 2 prepare (2,y)
        FBAEnvelope prepare1_y = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(2, yValue),
                                              FBAStatementType::PREPARE);
        prepare1_y.statement.body.prepare().prepared.activate() = 
            FBABallot(1, yValue);

        FBAEnvelope prepare2_y = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(2, yValue),
                                              FBAStatementType::PREPARE);
        prepare2_y.statement.body.prepare().prepared.activate() = 
            FBABallot(1, yValue);

        fba.receiveEnvelope(prepare1_y);
        REQUIRE(C.mEnvs.size() == 4);
        REQUIRE(C.mCancelledValues[0][0] == xValue);

        REQUIRE(C.mEnvs[3].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[3].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[3].statement.ballot.counter == 2);
        REQUIRE(C.mEnvs[3].statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(C.mEnvs[3].statement.body.prepare().excepted.size() == 1);
        REQUIRE(C.mEnvs[3].statement.body.prepare().excepted[0].counter == 0);
        REQUIRE(C.mEnvs[3].statement.body.prepare().excepted[0].value == xValue);
        REQUIRE(!C.mEnvs[3].statement.body.prepare().prepared);

        fba.receiveEnvelope(prepare2_y);
        REQUIRE(C.mEnvs.size() == 5);

        REQUIRE(C.mEnvs[4].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[4].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[4].statement.ballot.counter == 2);
        REQUIRE(C.mEnvs[4].statement.body.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_y = makeEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(2, yValue),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_y = makeEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(2, yValue),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_y);
        REQUIRE(C.mEnvs.size() == 5);

        fba.receiveEnvelope(prepared2_y);
        REQUIRE(C.mEnvs.size() == 6);

        REQUIRE(C.mEnvs[5].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[5].statement.ballot.value == yValue);
        REQUIRE(C.mEnvs[5].statement.ballot.counter == 2);
        REQUIRE(C.mEnvs[5].statement.body.type() == FBAStatementType::COMMIT);

        // finally things go south and we attempt x again
        REQUIRE(fba.attemptValue(0, xValue));
        REQUIRE(C.mEnvs.size() == 7);

        // we check the prepare message is all in order
        REQUIRE(C.mEnvs[6].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[6].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[6].statement.ballot.counter == 3);
        REQUIRE(C.mEnvs[6].statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(C.mEnvs[6].statement.body.prepare().excepted.size() == 2);
        REQUIRE(C.mEnvs[6].statement.body.prepare().excepted[0].counter == 0);
        REQUIRE(C.mEnvs[6].statement.body.prepare().excepted[0].value == xValue);
        REQUIRE(C.mEnvs[6].statement.body.prepare().excepted[1].counter == 2);
        REQUIRE(C.mEnvs[6].statement.body.prepare().excepted[1].value == yValue);
        REQUIRE(C.mEnvs[6].statement.body.prepare().prepared);
        REQUIRE((*C.mEnvs[6].statement.body.prepare().prepared).counter == 0);
        REQUIRE((*C.mEnvs[6].statement.body.prepare().prepared).value == xValue);
    }

    SECTION("missing prepare (0,y), retransmission hint")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, xValue),
                                            FBAStatementType::PREPARE);

        // never received prepare1
        fba.receiveEnvelope(prepare2);
        REQUIRE(C.mEnvs.size() == 1);

        REQUIRE(C.mEnvs[0].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[0].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[0].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[0].statement.body.type() == FBAStatementType::PREPARE);

        FBAEnvelope prepared1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, yValue),
                                             FBAStatementType::PREPARED);

        // the node must have hinted a retransmission
        fba.receiveEnvelope(prepared1);
        REQUIRE(C.mEnvs.size() == 1);
        REQUIRE(C.mRetransmissionHints[0].size() == 1);
        REQUIRE(C.mRetransmissionHints[0][0] == v1NodeID);

        // after retransmission all is in order
        fba.receiveEnvelope(prepare1);
        REQUIRE(C.mEnvs.size() == 2);

        REQUIRE(C.mEnvs[1].nodeID == v0NodeID);
        REQUIRE(C.mEnvs[1].statement.ballot.value == xValue);
        REQUIRE(C.mEnvs[1].statement.ballot.counter == 0);
        REQUIRE(C.mEnvs[1].statement.body.type() == FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared1);
        REQUIRE(C.mEnvs.size() == 2);
    }

    // TODO(spolu): add generic test to check that no statement emitted contradict
    //              itself
}




