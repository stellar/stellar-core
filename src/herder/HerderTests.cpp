// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "herder/Herder.h"
#include "fba/FBA.h"
#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "main/Config.h"
#include "simulation/Simulation.h"

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

TEST_CASE("standalone", "[herder]")
{
    SIMULATION_CREATE_NODE(0);

    Config cfg(getTestConfig());
    
    cfg.RUN_STANDALONE = true;
    cfg.VALIDATION_KEY = v0SecretKey;
    cfg.START_NEW_NETWORK = true;

    cfg.QUORUM_THRESHOLD = 1;
    cfg.QUORUM_SET.push_back(v0NodeID);

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    app->start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    const uint64_t paymentAmount = 
        (uint64_t)app->getLedgerMaster().getMinBalance(0);

    AccountFrame rootAccount;
    REQUIRE(AccountFrame::loadAccount(
        root.getPublicKey(), rootAccount, app->getDatabase()));
    
    SECTION("basic ledger close on valid txs")
    {
        bool stop = false;
        VirtualTimer setupTimer(app->getClock());
        VirtualTimer checkTimer(app->getClock());

        auto check = [&] (const asio::error_code& error)
        {
            stop = true;

            AccountFrame a1Account, b1Account;
            REQUIRE(AccountFrame::loadAccount(
                a1.getPublicKey(), a1Account, app->getDatabase()));
            REQUIRE(AccountFrame::loadAccount(
                b1.getPublicKey(), b1Account, app->getDatabase()));

            REQUIRE(a1Account.getBalance() == paymentAmount);
            REQUIRE(b1Account.getBalance() == paymentAmount);
        };

        auto setup = [&] (const asio::error_code& error)
        {
            // create accounts
            TransactionFramePtr txFrameA1 = createPaymentTx(root, a1, 1, paymentAmount);
            TransactionFramePtr txFrameA2 = createPaymentTx(root, b1, 2, paymentAmount);

            REQUIRE(app->getHerderGateway().recvTransaction(txFrameA1));
            REQUIRE(app->getHerderGateway().recvTransaction(txFrameA2));
        };

        setupTimer.expires_from_now(std::chrono::seconds(0));
        setupTimer.async_wait(setup);

        checkTimer.expires_from_now(std::chrono::seconds(5));
        checkTimer.async_wait(check);

        while(!stop && app->crank(false) > 0);
    }

}

// see if we flood at the right times
//  invalid tx
//  normal tx
//  tx with bad seq num
//  account can't pay for all the tx
//  account has just enough for all the tx
//  tx from account not in the DB
TEST_CASE("recvTx", "[herder]")
{
}

// sortForApply 
// sortForHash
// checkValid
//   not sorted correctly
//   tx with bad seq num
//   account can't pay for all the tx
//   account has just enough for all the tx
//   tx from account not in the DB 
TEST_CASE("txset", "[herder]")
{
}

