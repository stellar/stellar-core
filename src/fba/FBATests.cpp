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
    }
    void emitEnvelope(const FBAEnvelope& envelope)
    {
        //LOG(INFO) << xdr::xdr_to_string(envelope);
        mEmittedEnvelopes.push_back(envelope);
    }

    std::vector<FBAEnvelope> mEmittedEnvelopes;
    std::map<uint32, Hash> mExternalizedValues;
};

/* 
TEST_CASE("attemptValue", "[fba]")
{
    SECTION("first")
    {
        const Hash validationSeed = sha512_256("SEED_VALIDATIONSEED");

        FBAQuorumSet qSet;
        qSet.threshold = 3;
        for(int n = 0; n < 5; n++)
        {
            Hash nodeID = sha512_256("SEED_FOOBAR");;
            nodeID[0] = n;
            LOG(INFO) << "ADD TO QSET: " << binToHex(nodeID);
            qSet.validators.push_back(nodeID);
        }

        TestFBAClient* client = new TestFBAClient();

        FBA* fba = new FBA(validationSeed, qSet, client);

        const Hash valueHash = sha512_256("SEED_VALUEHASH");
        const Hash evidence;

        REQUIRE(fba->attemptValue(0, valueHash, evidence));
    }
}

*/


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

TEST_CASE("protocol core4", "[fba]")
{
    const Hash n0VSeed = sha512_256("SEED_VALIDATION_SEED_0");
    const Hash n1VSeed = sha512_256("SEED_VALIDATION_SEED_1");
    const Hash n2VSeed = sha512_256("SEED_VALIDATION_SEED_2");
    const Hash n3VSeed = sha512_256("SEED_VALIDATION_SEED_3");

    const Hash n0NodeID = makePublicKey(n0VSeed);
    const Hash n1NodeID = makePublicKey(n1VSeed);
    const Hash n2NodeID = makePublicKey(n2VSeed);
    const Hash n3NodeID = makePublicKey(n3VSeed);

    FBAQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(n0NodeID);
    qSet.validators.push_back(n1NodeID);
    qSet.validators.push_back(n2NodeID);
    qSet.validators.push_back(n3NodeID);

    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));

    class Core4FBAClient : public TestFBAClient
    {
      public:
        Core4FBAClient(const FBAQuorumSet& qSet)
          : mQSet(qSet)
        {
            mQSetHash = sha512_256(xdr::xdr_to_msg(qSet));
            LOG(INFO) << "Core4FBAClient " << binToHex(mQSetHash).substr(0,6);
        }

        void setFBA(FBA* FBA)
        {
            mFBA = FBA;
        }

        void retrieveQuorumSet(const uint256& nodeID,
                               const Hash& qSetHash)
        {
            LOG(INFO) << "FBA::Client::retrieveQuorumSet"
                      << " " << binToHex(qSetHash).substr(0,6)
                      << "@" << binToHex(nodeID).substr(0,6);

            if(qSetHash == mQSetHash)
            {
                LOG(INFO) << "receiveQuorumSet " 
                          << binToHex(qSetHash).substr(0,6);
                mFBA->receiveQuorumSet(nodeID, mQSet);
            }
        }

        FBAQuorumSet mQSet;
        Hash         mQSetHash;
        FBA*         mFBA;
    };

    Core4FBAClient* client = new Core4FBAClient(qSet);
    FBA* fba = new FBA(n0VSeed, qSet, client);
    client->setFBA(fba);

    const Hash xValueHash = sha512_256("SEED_VALUE_HASH_X");
    const Hash yValueHash = sha512_256("SEED_VALUE_HASH_X");
    const Hash zValueHash = sha512_256("SEED_VALUE_HASH_X");

    const Hash xEvidence = sha512_256("SEED_VALUE_EVIDENCE_X");
    const Hash yEvidence = sha512_256("SEED_VALUE_EVIDENCE_Y");
    const Hash zEvidence = sha512_256("SEED_VALUE_EVIDENCE_Z");

    /*
    SECTION("attemptValue x")
    {
        fba->attemptValue(0, xValueHash, xEvidence);

        FBAEnvelope last = client->mEmittedEnvelopes.back();

        REQUIRE(last.nodeID == n0NodeID);
        REQUIRE(last.statement.ballot.valueHash == xValueHash);
        REQUIRE(last.statement.ballot.counter == 0);
    }
    */

    SECTION("trivial round (0,x)")
    {
        FBAEnvelope prepare1 = createEnvelope(n1NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared1 = createEnvelope(n1NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope commit1 = createEnvelope(n1NodeID,
                                             qSetHash,
                                             0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::COMMIT);
        FBAEnvelope committed1 = createEnvelope(n1NodeID,
                                                qSetHash,
                                                0,
                                                FBABallot(0, xValueHash),
                                                FBAStatementType::COMMITTED);

        FBAEnvelope prepare2 = createEnvelope(n2NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared2 = createEnvelope(n2NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope commit2 = createEnvelope(n2NodeID,
                                             qSetHash,
                                             0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::COMMIT);
        FBAEnvelope committed2 = createEnvelope(n2NodeID,
                                                qSetHash,
                                                0,
                                                FBABallot(0, xValueHash),
                                                FBAStatementType::COMMITTED);

        FBAEnvelope prepare3 = createEnvelope(n3NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, xValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared3 = createEnvelope(n3NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, xValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope commit3 = createEnvelope(n3NodeID,
                                             qSetHash,
                                             0,
                                             FBABallot(0, xValueHash),
                                             FBAStatementType::COMMIT);
        FBAEnvelope committed3 = createEnvelope(n3NodeID,
                                                qSetHash,
                                                0,
                                                FBABallot(0, xValueHash),
                                                FBAStatementType::COMMITTED);

        fba->receiveEnvelope(prepare1);
        REQUIRE(client->mEmittedEnvelopes.size() == 1);

        // We send a prepare as we were pristine
        FBAEnvelope prepare = client->mEmittedEnvelopes[0];
        REQUIRE(prepare.nodeID == n0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);

        fba->receiveEnvelope(prepare2);
        REQUIRE(client->mEmittedEnvelopes.size() == 2);

        // We have a quorum including us
        FBAEnvelope prepared = client->mEmittedEnvelopes[1];
        REQUIRE(prepared.nodeID == n0NodeID);
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
        REQUIRE(commit.nodeID == n0NodeID);
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
        REQUIRE(committed.nodeID == n0NodeID);
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
        REQUIRE(ext.nodeID == n0NodeID);
        REQUIRE(ext.statement.ballot.valueHash == xValueHash);
        REQUIRE(ext.statement.ballot.counter == FBA_SLOT_MAX_COUNTER);
        REQUIRE(ext.statement.body.type() == FBAStatementType::COMMITTED);

        // We test there is no reemission of COMMITTED when a COMMITTED (inf,x)
        // is received
        FBAEnvelope ext3 = 
            createEnvelope(n3NodeID,
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

        FBAEnvelope prepare1 = createEnvelope(n1NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared1 = createEnvelope(n1NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, yValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepare2 = createEnvelope(n2NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared2 = createEnvelope(n2NodeID,
                                               qSetHash,
                                               0,
                                               FBABallot(0, yValueHash),
                                               FBAStatementType::PREPARED);
        FBAEnvelope prepare3 = createEnvelope(n3NodeID,
                                              qSetHash,
                                              0,
                                              FBABallot(0, yValueHash),
                                              FBAStatementType::PREPARE);
        FBAEnvelope prepared3 = createEnvelope(n3NodeID,
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

        REQUIRE(prepare.nodeID == n0NodeID);
        REQUIRE(prepare.statement.ballot.valueHash == xValueHash);
        REQUIRE(prepare.statement.ballot.counter == 0);
        REQUIRE(prepare.statement.body.type() == FBAStatementType::PREPARE);

        REQUIRE(prepared.nodeID == n0NodeID);
        REQUIRE(prepared.statement.ballot.valueHash == yValueHash);
        REQUIRE(prepared.statement.ballot.counter == 0);
        REQUIRE(prepared.statement.body.type() == FBAStatementType::PREPARED);

        REQUIRE(commit.nodeID == n0NodeID);
        REQUIRE(commit.statement.ballot.valueHash == yValueHash);
        REQUIRE(commit.statement.ballot.counter == 0);
        REQUIRE(commit.statement.body.type() == FBAStatementType::COMMIT);
    }

}




