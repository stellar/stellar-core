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
        LOG(INFO) << "FBA::Client::retrieveQuorumSet"
                  << " " << binToHex(qSetHash).substr(0,6)
                  << "@" << binToHex(nodeID).substr(0,6);

        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {
            LOG(INFO) << "receiveQuorumSet " 
                      << "@" << binToHex(nodeID).substr(0,6)
                      << " " << binToHex(qSetHash).substr(0,6);
            mFBA->receiveQuorumSet(nodeID, mQuorumSets[qSetHash]);
        }
    }
    void emitEnvelope(const FBAEnvelope& envelope)
    {
        mEmittedEnvelopes.push_back(envelope);
    }

    std::map<Hash, FBAQuorumSet> mQuorumSets;
    std::vector<FBAEnvelope>     mEmittedEnvelopes;
    std::map<uint32, Hash>       mExternalizedValues;
    FBA*                         mFBA;
};


FBAEnvelope createEnvelope(const uint256& nodeID,
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

    TestFBAClient* client = new TestFBAClient();
    FBA* fba = new FBA(v0VSeed, qSet, client);

    client->setFBA(fba);
    client->addQuorumSet(qSet);

    CREATE_VALUE(x);
    CREATE_VALUE(y);
    CREATE_VALUE(z);

    SECTION("attemptValue x")
    {
        REQUIRE(fba->attemptValue(0, xValueHash, xEvidence));

        REQUIRE(client->mEmittedEnvelopes.size() == 1);

        FBAEnvelope prepare = client->mEmittedEnvelopes[0];
        REQUIRE(prepare.nodeID == v0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);
    }

    SECTION("trivial round (0,x)")
    {
        FBAEnvelope prepare1 = createEnvelope(v1NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared1 = createEnvelope(v1NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope commit1 = createEnvelope(v1NodeID,
                                             qSetHash,
                                             0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::COMMIT);
        FBAEnvelope committed1 = createEnvelope(v1NodeID,
                                                qSetHash,
                                                0,
                                                FBABallot(0, xValueHash),
                                                FBAStatementType::COMMITTED);

        FBAEnvelope prepare2 = createEnvelope(v2NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared2 = createEnvelope(v2NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope commit2 = createEnvelope(v2NodeID,
                                             qSetHash,
                                             0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::COMMIT);
        FBAEnvelope committed2 = createEnvelope(v2NodeID,
                                                qSetHash,
                                                0,
                                                FBABallot(0, xValueHash),
                                                FBAStatementType::COMMITTED);

        FBAEnvelope prepare3 = createEnvelope(v3NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared3 = createEnvelope(v3NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope commit3 = createEnvelope(v3NodeID,
                                             qSetHash,
                                             0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::COMMIT);
        FBAEnvelope committed3 = createEnvelope(v3NodeID,
                                                qSetHash,
                                                0,
                                                FBABallot(0, xValueHash),
                                                FBAStatementType::COMMITTED);

        fba->receiveEnvelope(prepare1);
        REQUIRE(client->mEmittedEnvelopes.size() == 1);

        // We send a prepare as we were pristine
        FBAEnvelope prepare = client->mEmittedEnvelopes[0];
        REQUIRE(prepare.nodeID == v0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);

        fba->receiveEnvelope(prepare2);
        REQUIRE(client->mEmittedEnvelopes.size() == 2);

        // We have a quorum including us
        FBAEnvelope prepared = client->mEmittedEnvelopes[1];
        REQUIRE(prepared.nodeID == v0NodeID);
        REQUIRE(prepared.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepared.statement.ballot.counter == 0);
        REQUIRE(prepared.statement.body.type() == FBAStatementType::PREPARED);

        fba->receiveEnvelope(prepare3);
        REQUIRE(client->mEmittedEnvelopes.size() == 2);

        fba->receiveEnvelope(prepared3);
        REQUIRE(client->mEmittedEnvelopes.size() == 2);

        fba->receiveEnvelope(prepared2);
        REQUIRE(client->mEmittedEnvelopes.size() == 3);

        FBAEnvelope commit = client->mEmittedEnvelopes[2];
        REQUIRE(commit.nodeID == v0NodeID);
        REQUIRE(commit.statement.ballot.valueHash == xValueHash);
        REQUIRE(commit.statement.ballot.counter == 0);
        REQUIRE(commit.statement.body.type() == FBAStatementType::COMMIT);
        
        fba->receiveEnvelope(prepared1);
        REQUIRE(client->mEmittedEnvelopes.size() == 3);

        fba->receiveEnvelope(commit2);
        REQUIRE(client->mEmittedEnvelopes.size() == 3);

        fba->receiveEnvelope(commit1);
        REQUIRE(client->mEmittedEnvelopes.size() == 4);

        FBAEnvelope committed = client->mEmittedEnvelopes[3];
        REQUIRE(committed.nodeID == v0NodeID);
        REQUIRE(committed.statement.ballot.valueHash == xValueHash);
        REQUIRE(committed.statement.ballot.counter == 0);
        REQUIRE(committed.statement.body.type() == FBAStatementType::COMMITTED);

        fba->receiveEnvelope(commit3);
        REQUIRE(client->mEmittedEnvelopes.size() == 4);

        fba->receiveEnvelope(committed3);
        REQUIRE(client->mEmittedEnvelopes.size() == 4);
        // The slot should not have externalized yet
        REQUIRE(client->mExternalizedValues.find(0) == 
            client->mExternalizedValues.end());

        fba->receiveEnvelope(committed1);
        REQUIRE(client->mEmittedEnvelopes.size() == 4);
        // The slot should have externalized the value
        REQUIRE(client->mExternalizedValues[0] == xValueHash);

        fba->receiveEnvelope(committed2);
        REQUIRE(client->mEmittedEnvelopes.size() == 5);

        FBAEnvelope ext = client->mEmittedEnvelopes[4];
        REQUIRE(ext.nodeID == v0NodeID);
        REQUIRE(ext.statement.ballot.valueHash == xValueHash);
        REQUIRE(ext.statement.ballot.counter == FBA_SLOT_MAX_COUNTER);
        REQUIRE(ext.statement.body.type() == FBAStatementType::COMMITTED);

        // We test there is no reemission of COMMITTED when a COMMITTED (inf,x)
        // is received
        FBAEnvelope ext3 = 
            createEnvelope(v3NodeID,
                           qSetHash,
                           0,
                           FBABallot(FBA_SLOT_MAX_COUNTER, xValueHash),
                           FBAStatementType::COMMITTED);
        fba->receiveEnvelope(ext3);
        REQUIRE(client->mEmittedEnvelopes.size() == 5);
    }

    SECTION("attempt (0,x), prepared (0,y) by v-blocking")
    {
        fba->attemptValue(0, xValueHash, xEvidence);

        FBAEnvelope prepare1 = createEnvelope(v1NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared1 = createEnvelope(v1NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, yValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepare2 = createEnvelope(v2NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared2 = createEnvelope(v2NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, yValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepare3 = createEnvelope(v3NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared3 = createEnvelope(v3NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, yValueHash),
                                               FBAStatementType::PREPARED);
        fba->receiveEnvelope(prepare1);
        fba->receiveEnvelope(prepare2);
        fba->receiveEnvelope(prepare3);
        fba->receiveEnvelope(prepared1);
        fba->receiveEnvelope(prepared2);

        FBAEnvelope prepare = client->mEmittedEnvelopes[0];
        FBAEnvelope prepared = client->mEmittedEnvelopes[1];
        FBAEnvelope commit = client->mEmittedEnvelopes[2];

        REQUIRE(prepare.nodeID == v0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);

        REQUIRE(prepared.nodeID == v0NodeID);
        REQUIRE(prepared.statement.ballot.valueHash == yValueHash);
        REQUIRE(prepared.statement.ballot.counter == 0);
        REQUIRE(prepared.statement.body.type() == FBAStatementType::PREPARED);

        REQUIRE(commit.nodeID == v0NodeID);
        REQUIRE(commit.statement.ballot.valueHash == yValueHash);
        REQUIRE(commit.statement.ballot.counter == 0);
        REQUIRE(commit.statement.body.type() == FBAStatementType::COMMIT);
    }

}




