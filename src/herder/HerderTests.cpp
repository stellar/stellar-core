// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "herder/Herder.h"
#include "fba/FBA.h"
#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "main/Config.h"

#include <cassert>
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "transactions/TxTests.h"
#include "database/Database.h"


using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

#define CREATE_NODE(N) \
    const Hash v##N##VSeed = sha512_256("SEED_VALIDATION_SEED_" #N); \
    const Hash v##N##NodeID = makePublicKey(v##N##VSeed);

// see if we flood at the right times
//  invalid tx
//  normal tx
//  tx with bad seq num
//  account can't pay for all the tx
//  account has just enough for all the tx
//  tx from account not in the DB
TEST_CASE("recvTx", "[hrd]")
{
    CREATE_NODE(0);
    CREATE_NODE(1);
    CREATE_NODE(2);
    CREATE_NODE(3);

    FBAQuorumSetPtr qSet = std::make_shared<FBAQuorumSet>();
    qSet->threshold = 3;
    qSet->validators.push_back(v0NodeID);
    qSet->validators.push_back(v1NodeID);
    qSet->validators.push_back(v2NodeID);
    qSet->validators.push_back(v3NodeID);

    Config cfg(getTestConfig());
    
    cfg.RUN_STANDALONE = true;
    //cfg.START_NEW_NETWORK = true;

    cfg.QUORUM_THRESHOLD = qSet->threshold;
    cfg.QUORUM_SET.push_back(v0NodeID);
    cfg.QUORUM_SET.push_back(v1NodeID);
    cfg.QUORUM_SET.push_back(v2NodeID);
    cfg.QUORUM_SET.push_back(v3NodeID);

    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    const uint64_t paymentAmount = (uint64_t)app.getLedgerMaster().getMinBalance(0);
    // create accounts
    applyPaymentTx(app, root, a1, 1, paymentAmount);
    applyPaymentTx(app, root, a1, 2, paymentAmount);

    // TODO(spolu) HerderTests recvTx
}


// sortForApply 
// sortForHash
// checkValid
//   not sorted correctly
//   tx with bad seq num
//   account can't pay for all the tx
//   account has just enough for all the tx
//   tx from account not in the DB 
TEST_CASE("txset", "[hrd]")
{

}

