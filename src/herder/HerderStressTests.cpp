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
using namespace std;

typedef shared_ptr<Application> appPtr;


struct PeerInfo {
	SecretKey peerKey;
	SecretKey validationKey;
	int peerPort;
};

appPtr
createApp(Config &baseConfig, VirtualClock &clock, int nValidationPeers, int i, PeerInfo &me, vector<PeerInfo> &peers) {
	Config cfg = baseConfig;
	cfg.PEER_KEY = me.peerKey;
	cfg.PEER_PUBLIC_KEY = me.peerKey.getPublicKey();
	cfg.VALIDATION_KEY = me.validationKey;
	cfg.PEER_PORT = me.peerPort;
	cfg.HTTP_PORT = me.peerPort + 1;

	cfg.LOG_FILE_PATH = cfg.LOG_FILE_PATH.substr(0, cfg.LOG_FILE_PATH.size() - 4) + "-node-" + to_string(i) + ".cfg";
	cfg.DATABASE = "sqlite3://stellar-hrd-test-node-" + to_string(i) + ".db";

	cfg.QUORUM_THRESHOLD = min(nValidationPeers / 2 + 4, nValidationPeers);
	cfg.PREFERRED_PEERS.clear();
	cfg.QUORUM_SET.clear();
	for (auto peer : peers) {
		cfg.PREFERRED_PEERS.push_back("127.0.0.1:" + to_string(peer.peerPort));
		cfg.QUORUM_SET.push_back(peer.validationKey.getPublicKey());
	}
	cfg.KNOWN_PEERS.clear();

	return	make_shared<Application>(clock, cfg);
}


shared_ptr<vector<appPtr>>
createApps(Config &baseConfig, VirtualClock &clock, int n, int nValidationPeers) {
	vector<PeerInfo> peers;

	for (int i = 0; i < n; i++) {
		peers.push_back(PeerInfo { SecretKey::random(), SecretKey::random(), baseConfig.PEER_PORT + i * 2 });
	}

	auto result = make_shared<vector<appPtr>>();

	for (int i = 0; i < n; i++) {
		vector<PeerInfo> myPeers;
		if (i < nValidationPeers) {
			// The first few nodes depend on the next `nValidationPeers` ones.
			myPeers = vector<PeerInfo>(peers.begin() + i + 1, peers.begin() + i + 1 + nValidationPeers);
		}
		else {
			// The other nodes depend on the `nValidationPeers` previous ones.
			myPeers = vector<PeerInfo>(peers.begin() + i - nValidationPeers, peers.begin() + i);
		}
		result->push_back(createApp(baseConfig, clock, nValidationPeers, i, peers[i], myPeers));
	}

	return result;
}

TEST_CASE("stress", "[hrd]")
{
    VirtualClock clock;
    Config cfg(getTestConfig());
	createApps(cfg, clock, 20, 3);

	/*
    SIMULATION_CREATE_NODE(0);

    
    cfg.RUN_STANDALONE = true;
    cfg.VALIDATION_KEY = v0SecretKey;
    cfg.START_NEW_NETWORK = true;

    cfg.QUORUM_THRESHOLD = 1;
    cfg.QUORUM_SET.push_back(v0NodeID);

    Application app(clock, cfg);

    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");

    const uint64_t paymentAmount = 
        (uint64_t)app.getLedgerMaster().getMinBalance(0);

    AccountFrame rootAccount;
    REQUIRE(AccountFrame::loadAccount(
        root.getPublicKey(), rootAccount, app.getDatabase()));
    
    SECTION("basic ledger close on valid tx")
    {
        bool stop = false;
        VirtualTimer setupTimer(app.getClock());
        VirtualTimer checkTimer(app.getClock());

        auto check = [&] (const asio::error_code& error)
        {
            stop = true;

            AccountFrame a1Account;
            REQUIRE(AccountFrame::loadAccount(
                a1.getPublicKey(), a1Account, app.getDatabase()));

            REQUIRE(a1Account.getBalance() == paymentAmount);
        };

        auto setup = [&] (const asio::error_code& error)
        {
            // create account
            TransactionFramePtr txFrameA1 = 
                createPaymentTx(root, a1, 1, paymentAmount);

            REQUIRE(app.getHerderGateway().recvTransaction(txFrameA1));
        };

        setupTimer.expires_from_now(std::chrono::seconds(0));
        setupTimer.async_wait(setup);

        checkTimer.expires_from_now(std::chrono::seconds(2));
        checkTimer.async_wait(check);

        while(!stop && app.crank(false) > 0);
    }
*/
}
