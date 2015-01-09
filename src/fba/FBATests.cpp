// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "lib/catch.hpp"
#include "fba/FBA.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"

using namespace stellar;

class TestFBAClient : public FBA::Client
{
    void validateBallot(const uint32& slotIndex,
                        const Hash& nodeID,
                        const FBABallot& ballot,
                        std::function<void(bool)> const& cb)
    {
        cb(true);
    }

    void ballotDidPrepare(const uint32& slotIndex,
                          const FBABallot& ballot)
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
        LOG(INFO) << "Envelope emitted";
    }
};

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
        fba->attemptValue(0, valueHash);
        LOG(INFO) << "TEST";
    }
}



