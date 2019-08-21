// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/DatabaseImpl.h"
#include "herder/HerderImpl.h"
#include "herder/HerderPersistenceImpl.h"
#include "herder/LedgerCloseData.h"
#include "history/HistoryManagerImpl.h"
#include "ledger/LedgerManagerImpl.h"
#include "lib/catch.hpp"
#include "main/RelayApplication.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerManagerImpl.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/test/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "work/WorkScheduler.h"

using namespace stellar;

TEST_CASE("Relay applications in loopback mode", "[relay]")
{
    VirtualClock clock;
    auto cfg0 = getTestConfig(0);
    auto cfg1 = getTestConfig(1);
    cfg0.HTTP_PORT = 0;
    cfg1.HTTP_PORT = 0;

    std::shared_ptr<RelayApplication> app1 =
        Application::create<RelayApplication>(
            clock, cfg0, Application::InitialDBMode::APP_DB_DONT_OPEN);

    std::shared_ptr<RelayApplication> app2 =
        Application::create<RelayApplication>(
            clock, cfg1, Application::InitialDBMode::APP_DB_DONT_OPEN);

    app1->start();
    app2->start();

    std::vector<std::shared_ptr<RelayApplication>> nodes{app1, app2};

    LoopbackPeerConnection connection(*app1, *app2);
    auto peer1 = connection.getInitiator();
    auto peer2 = connection.getAcceptor();

    SecretKey dest = SecretKey::pseudoRandomForTesting();
    auto injectTransaction = [&](int i) {
        const int64 txAmount = 10000000;

        // round robin
        auto inApp = nodes[i % nodes.size()];
        auto account =
            TestAccount{*inApp, SecretKey::pseudoRandomForTesting(), 1};
        auto tx1 = account.tx(
            {txtest::createAccount(dest.getPublicKey(), txAmount)}, 1);

        // this is basically a modified version of Peer::recvTransaction
        auto msg = tx1->toStellarMessage();
        auto res = inApp->getHerder().recvTransaction(tx1);
        REQUIRE(res == TransactionQueue::AddResult::ADD_STATUS_PENDING);
        inApp->getOverlayManager().broadcastMessage(msg);
    };

    int i = 0;
    while (i < 100 && clock.crank(false) > 0 &&
           !app1->getOverlayManager().isShuttingDown())
    {
        injectTransaction(i++);
        clock.crank(false);
        clock.crank(false);
        clock.crank(false);
    }
}
