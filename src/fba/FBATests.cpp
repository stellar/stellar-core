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

    void validateBallot(const uint32& slotIndex,
                        const Hash& nodeID,
                        const FBABallot& ballot,
                        const Hash& evidence,
                        std::function<void(bool)> const& cb)
    {
        cb(true);
    }

    void ballotDidPrepare(const uint32& slotIndex,
                          const FBABallot& ballot,
                          const Hash& evidence)
    {
    }
    void ballotDidCommit(const uint32& slotIndex,
                         const FBABallot& ballot)
    {
    }

    void valueCancelled(const uint32& slotIndex,
                        const Hash& valueHash)
    {
        mCancelledValues[slotIndex].push_back(valueHash);
    }

    void valueExternalized(const uint32& slotIndex,
                           const Hash& valueHash)
    {
        if (mExternalizedValues.find(slotIndex) != mExternalizedValues.end())
        {
            throw std::out_of_range("Value already externalized");
        }
        mExternalizedValues[slotIndex] = valueHash;
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
        mEmittedEnvelopes.push_back(envelope);
    }

    std::map<Hash, FBAQuorumSet>        mQuorumSets;
    std::vector<FBAEnvelope>            mEmittedEnvelopes;
    std::map<uint32, Hash>              mExternalizedValues;
    std::map<uint32, std::vector<Hash>> mCancelledValues;
    FBA*                                mFBA;
};


FBAEnvelope 
makeEnvelope(const uint256& nodeID,
             const Hash& qSetHash,
             const uint32& slotIndex,
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
    const Hash X##Evidence = sha512_256("SEED_VALUE_EVIDENCE_" #X);

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

    TestFBAClient client;
    FBA fba(v0VSeed, qSet, &client);

    client.setFBA(&fba);
    client.addQuorumSet(qSet);

    CREATE_VALUE(x);
    CREATE_VALUE(y);
    CREATE_VALUE(z);

    SECTION("attemptValue x")
    {
        REQUIRE(fba.attemptValue(0, xValueHash, xEvidence));

        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        FBAEnvelope prepare = client.mEmittedEnvelopes[0];
        REQUIRE(prepare.nodeID == v0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);
    }

    SECTION("normal round (0,x)")
    {
        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, xValueHash),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, xValueHash),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                            FBABallot(0, xValueHash),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        // We send a prepare as we were pristine
        FBAEnvelope prepare = client.mEmittedEnvelopes[0];
        REQUIRE(prepare.nodeID == v0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare2);
        REQUIRE(client.mEmittedEnvelopes.size() == 2);

        // We have a quorum including us
        FBAEnvelope prepared = client.mEmittedEnvelopes[1];
        REQUIRE(prepared.nodeID == v0NodeID);
        REQUIRE(prepared.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepared.statement.ballot.counter == 0);
        REQUIRE(prepared.statement.body.type() == FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepare3);
        REQUIRE(client.mEmittedEnvelopes.size() == 2);

        FBAEnvelope prepared1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::PREPARED);

        fba.receiveEnvelope(prepared3);
        REQUIRE(client.mEmittedEnvelopes.size() == 2);

        fba.receiveEnvelope(prepared2);
        REQUIRE(client.mEmittedEnvelopes.size() == 3);

        FBAEnvelope commit = client.mEmittedEnvelopes[2];
        REQUIRE(commit.nodeID == v0NodeID);
        REQUIRE(commit.statement.ballot.valueHash == xValueHash);
        REQUIRE(commit.statement.ballot.counter == 0);
        REQUIRE(commit.statement.body.type() == FBAStatementType::COMMIT);
        
        fba.receiveEnvelope(prepared1);
        REQUIRE(client.mEmittedEnvelopes.size() == 3);

        FBAEnvelope commit1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                           FBABallot(0, xValueHash),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                           FBABallot(0, xValueHash),
                                           FBAStatementType::COMMIT);
        FBAEnvelope commit3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                           FBABallot(0, xValueHash),
                                           FBAStatementType::COMMIT);

        fba.receiveEnvelope(commit2);
        REQUIRE(client.mEmittedEnvelopes.size() == 3);

        fba.receiveEnvelope(commit1);
        REQUIRE(client.mEmittedEnvelopes.size() == 4);

        FBAEnvelope committed = client.mEmittedEnvelopes[3];
        REQUIRE(committed.nodeID == v0NodeID);
        REQUIRE(committed.statement.ballot.valueHash == xValueHash);
        REQUIRE(committed.statement.ballot.counter == 0);
        REQUIRE(committed.statement.body.type() == FBAStatementType::COMMITTED);

        fba.receiveEnvelope(commit3);
        REQUIRE(client.mEmittedEnvelopes.size() == 4);

        FBAEnvelope committed1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed3);
        REQUIRE(client.mEmittedEnvelopes.size() == 4);
        // The slot should not have externalized yet
        REQUIRE(client.mExternalizedValues.find(0) == 
            client.mExternalizedValues.end());

        fba.receiveEnvelope(committed1);
        REQUIRE(client.mEmittedEnvelopes.size() == 4);
        // The slot should have externalized the value
        REQUIRE(client.mExternalizedValues[0] == xValueHash);

        fba.receiveEnvelope(committed2);
        REQUIRE(client.mEmittedEnvelopes.size() == 5);

        FBAEnvelope ext = client.mEmittedEnvelopes[4];
        REQUIRE(ext.nodeID == v0NodeID);
        REQUIRE(ext.statement.ballot.valueHash == xValueHash);
        REQUIRE(ext.statement.ballot.counter == FBA_SLOT_MAX_COUNTER);
        REQUIRE(ext.statement.body.type() == FBAStatementType::COMMITTED);

        // We test there is no reemission of COMMITTED when a COMMITTED (inf,x)
        // is received
        FBAEnvelope externalize3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                                FBABallot(FBA_SLOT_MAX_COUNTER, 
                                                          xValueHash),
                                                FBAStatementType::COMMITTED);

        fba.receiveEnvelope(externalize3);
        REQUIRE(client.mEmittedEnvelopes.size() == 5);
    }

    SECTION("attempt (0,x), prepared (0,y) by v-blocking")
    {
        REQUIRE(fba.attemptValue(0, xValueHash, xEvidence));

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(0, yValueHash),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, yValueHash),
                                            FBAStatementType::PREPARE);
        FBAEnvelope prepare3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                            FBABallot(0, yValueHash),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        fba.receiveEnvelope(prepare2);
        fba.receiveEnvelope(prepare3);

        FBAEnvelope prepared1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                             FBABallot(0, yValueHash),
                                             FBAStatementType::PREPARED);
        FBAEnvelope prepared2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                             FBABallot(0, yValueHash),
                                             FBAStatementType::PREPARED);

        FBAEnvelope prepare = client.mEmittedEnvelopes[0];
        REQUIRE(prepare.nodeID == v0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepared1);
        fba.receiveEnvelope(prepared2);

        FBAEnvelope prepared = client.mEmittedEnvelopes[1];
        FBAEnvelope commit = client.mEmittedEnvelopes[2];

        REQUIRE(prepared.nodeID == v0NodeID);
        REQUIRE(prepared.statement.ballot.valueHash == yValueHash);
        REQUIRE(prepared.statement.ballot.counter == 0);
        REQUIRE(prepared.statement.body.type() == FBAStatementType::PREPARED);

        REQUIRE(commit.nodeID == v0NodeID);
        REQUIRE(commit.statement.ballot.valueHash == yValueHash);
        REQUIRE(commit.statement.ballot.counter == 0);
        REQUIRE(commit.statement.body.type() == FBAStatementType::COMMIT);
    }

    SECTION("attempt (0,x), committed (inf,y) by quorum")
    {
        REQUIRE(fba.attemptValue(0, xValueHash, xEvidence));
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        FBAEnvelope committed1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(FBA_SLOT_MAX_COUNTER, 
                                                        yValueHash),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(FBA_SLOT_MAX_COUNTER,
                                                        yValueHash),
                                              FBAStatementType::COMMITTED);
        FBAEnvelope committed3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(FBA_SLOT_MAX_COUNTER,
                                                        yValueHash),
                                              FBAStatementType::COMMITTED);

        fba.receiveEnvelope(committed1);
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        fba.receiveEnvelope(committed2);
        // TODO(spolu): Should we be able to move directly onto COMMITTED, as 
        //              we see a v-blocking set of node and therefore move
        //              onto externalization as we have quorum with ourselves?
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        fba.receiveEnvelope(committed3);
        // The slot should have externalized the value
        REQUIRE(client.mExternalizedValues[0] == yValueHash);
    }

    SECTION("attempt (0,x), prepare (1,y), * (0,z)")
    {
        REQUIRE(fba.attemptValue(0, xValueHash, xEvidence));
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                            FBABallot(1, yValueHash),
                                            FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1);
        REQUIRE(client.mEmittedEnvelopes.size() == 2);
        REQUIRE(client.mCancelledValues[0][0] == xValueHash);

        FBAEnvelope prepare2 = makeEnvelope(v2NodeID, qSetHash, 0,
                                            FBABallot(0, zValueHash),
                                            FBAStatementType::PREPARE);
        fba.receiveEnvelope(prepare2);

        FBAEnvelope commit3 = makeEnvelope(v3NodeID, qSetHash, 0,
                                           FBABallot(0, zValueHash),
                                           FBAStatementType::COMMIT);
        fba.receiveEnvelope(commit3);

        // The envelopes should be ignored (no prepare)
        REQUIRE(client.mEmittedEnvelopes.size() == 2);
    }

    SECTION("pledge to commit (0,x), accept cancel on (1,y)")
    {
        // 1 and 2 prepare (0,x)
        FBAEnvelope prepare1_x = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepare2_x = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);

        // 3 prepares (0,y)
        FBAEnvelope prepare3_y = makeEnvelope(v3NodeID, qSetHash, 0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare1_x);
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        FBAEnvelope prepare_x = client.mEmittedEnvelopes[0];
        REQUIRE(prepare_x.nodeID == v0NodeID);
        REQUIRE(prepare_x.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare_x.statement.ballot.counter == 0);
        REQUIRE(prepare_x.statement.body.type() == FBAStatementType::PREPARE);

        fba.receiveEnvelope(prepare3_y);
        REQUIRE(client.mEmittedEnvelopes.size() == 1);

        fba.receiveEnvelope(prepare2_x);
        REQUIRE(client.mEmittedEnvelopes.size() == 2);

        FBAEnvelope prepared_x = client.mEmittedEnvelopes[1];
        REQUIRE(prepared_x.nodeID == v0NodeID);
        REQUIRE(prepared_x.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepared_x.statement.ballot.counter == 0);
        REQUIRE(prepared_x.statement.body.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_x = makeEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_x = makeEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_x);
        REQUIRE(client.mEmittedEnvelopes.size() == 2);

        fba.receiveEnvelope(prepared2_x);
        REQUIRE(client.mEmittedEnvelopes.size() == 3);

        FBAEnvelope commit_x = client.mEmittedEnvelopes[2];
        REQUIRE(commit_x.nodeID == v0NodeID);
        REQUIRE(commit_x.statement.ballot.valueHash == xValueHash);
        REQUIRE(commit_x.statement.ballot.counter == 0);
        REQUIRE(commit_x.statement.body.type() == FBAStatementType::COMMIT);

        // then our node becomes laggy for a while time during which 1, 2, 3
        // confirms prepare on (1,y) but 3 dies. Then we come back and 2
        // prepares (2,y)

        // 1 and 2 prepare (2,y)
        FBAEnvelope prepare1_y = makeEnvelope(v1NodeID, qSetHash, 0,
                                              FBABallot(2, yValueHash),
                                              FBAStatementType::PREPARE);
        prepare1_y.statement.body.prepare().prepared.activate() = 
            FBABallot(1, yValueHash);

        FBAEnvelope prepare2_y = makeEnvelope(v2NodeID, qSetHash, 0,
                                              FBABallot(2, yValueHash),
                                              FBAStatementType::PREPARE);
        prepare2_y.statement.body.prepare().prepared.activate() = 
            FBABallot(1, yValueHash);

        fba.receiveEnvelope(prepare1_y);
        REQUIRE(client.mEmittedEnvelopes.size() == 4);
        REQUIRE(client.mCancelledValues[0][0] == xValueHash);

        FBAEnvelope prepare_y = client.mEmittedEnvelopes[3];
        REQUIRE(prepare_y.nodeID == v0NodeID);
        REQUIRE(prepare_y.statement.ballot.valueHash == yValueHash);
        REQUIRE(prepare_y.statement.ballot.counter == 2);
        REQUIRE(prepare_y.statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(prepare_y.statement.body.prepare().excepted.size() == 1);
        REQUIRE(prepare_y.statement.body.prepare().excepted[0].counter == 0);
        REQUIRE(prepare_y.statement.body.prepare().excepted[0].valueHash == 
            xValueHash);
        REQUIRE(!prepare_y.statement.body.prepare().prepared);

        fba.receiveEnvelope(prepare2_y);
        REQUIRE(client.mEmittedEnvelopes.size() == 5);

        FBAEnvelope prepared_y = client.mEmittedEnvelopes[4];
        REQUIRE(prepared_y.nodeID == v0NodeID);
        REQUIRE(prepared_y.statement.ballot.valueHash == yValueHash);
        REQUIRE(prepared_y.statement.ballot.counter == 2);
        REQUIRE(prepared_y.statement.body.type() == FBAStatementType::PREPARED);

        FBAEnvelope prepared1_y = makeEnvelope(v1NodeID, qSetHash, 0,
                                               FBABallot(2, yValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepared2_y = makeEnvelope(v2NodeID, qSetHash, 0,
                                               FBABallot(2, yValueHash),
                                               FBAStatementType::PREPARED);
        
        fba.receiveEnvelope(prepared1_y);
        REQUIRE(client.mEmittedEnvelopes.size() == 5);

        fba.receiveEnvelope(prepared2_y);
        REQUIRE(client.mEmittedEnvelopes.size() == 6);

        FBAEnvelope commit_y = client.mEmittedEnvelopes[5];
        REQUIRE(commit_y.nodeID == v0NodeID);
        REQUIRE(commit_y.statement.ballot.valueHash == yValueHash);
        REQUIRE(commit_y.statement.ballot.counter == 2);
        REQUIRE(commit_y.statement.body.type() == FBAStatementType::COMMIT);

        // finally things go south and we attempt x again
        REQUIRE(fba.attemptValue(0, xValueHash, xEvidence));
        REQUIRE(client.mEmittedEnvelopes.size() == 7);

        // we check the prepare message is all in order
        FBAEnvelope prepare_x2 = client.mEmittedEnvelopes[6];
        REQUIRE(prepare_x2.nodeID == v0NodeID);
        REQUIRE(prepare_x2.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare_x2.statement.ballot.counter == 3);
        REQUIRE(prepare_x2.statement.body.type() == FBAStatementType::PREPARE);
        REQUIRE(prepare_x2.statement.body.prepare().excepted.size() == 2);
        REQUIRE(prepare_x2.statement.body.prepare().excepted[0].counter == 0);
        REQUIRE(prepare_x2.statement.body.prepare().excepted[0].valueHash == 
            xValueHash);
        REQUIRE(prepare_x2.statement.body.prepare().excepted[1].counter == 2);
        REQUIRE(prepare_x2.statement.body.prepare().excepted[1].valueHash == 
            yValueHash);
        REQUIRE(prepare_x2.statement.body.prepare().prepared);
        REQUIRE((*prepare_x2.statement.body.prepare().prepared).counter == 0);
        REQUIRE((*prepare_x2.statement.body.prepare().prepared).valueHash == 
            xValueHash);
    }
}




