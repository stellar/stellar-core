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

using appPtr = shared_ptr<Application>;

struct PeerInfo {
    SecretKey peerKey;
    SecretKey validationKey;
    int peerPort;
};

appPtr
createApp(Config &baseConfig, VirtualClock &clock, int nValidationPeers, int i, PeerInfo &me, vector<PeerInfo> &peers) 
{
    Config cfg = baseConfig;
    cfg.PEER_KEY = me.peerKey;
    cfg.PEER_PUBLIC_KEY = me.peerKey.getPublicKey();
    cfg.VALIDATION_KEY = me.validationKey;
    cfg.PEER_PORT = me.peerPort;
    cfg.HTTP_PORT = me.peerPort + 1;

    auto nodeStr = "-node-" + to_string(i);
    cfg.LOG_FILE_PATH = cfg.LOG_FILE_PATH.substr(0, cfg.LOG_FILE_PATH.size() - 4) + nodeStr + ".cfg";
    cfg.DATABASE = "sqlite3://stellar-hrd-test" + nodeStr + ".db";
    cfg.TMP_DIR_PATH = cfg.TMP_DIR_PATH + "/tmp" + nodeStr;

    cfg.QUORUM_THRESHOLD = min(nValidationPeers / 2 + 4, nValidationPeers);
    cfg.PREFERRED_PEERS.clear();
    cfg.QUORUM_SET.clear();
    for (auto peer : peers) 
    {
        cfg.PREFERRED_PEERS.push_back("127.0.0.1:" + to_string(peer.peerPort));
        cfg.QUORUM_SET.push_back(peer.validationKey.getPublicKey());
    }
    cfg.KNOWN_PEERS.clear();

    return  make_shared<Application>(clock, cfg);
}


shared_ptr<vector<appPtr>>
createApps(Config &baseConfig, VirtualClock &clock, int n, int quorumThresold) 
{
    vector<PeerInfo> peers;

    for (int i = 0; i < n; i++) 
    {
        peers.push_back(PeerInfo { SecretKey::random(), SecretKey::random(), baseConfig.PEER_PORT + i * 2 });
    }

    auto result = make_shared<vector<appPtr>>();

    for (int i = 0; i < n; i++) 
    {
        vector<PeerInfo> myPeers;
        if (i < quorumThresold) 
        {
            // The first few nodes depend on the next `nValidationPeers` ones.
            myPeers = vector<PeerInfo>(peers.begin() + i + 1, peers.begin() + i + 1 + quorumThresold);
        }
        else {
            // The other nodes depend on the `nValidationPeers` previous ones.
            myPeers = vector<PeerInfo>(peers.begin() + i - quorumThresold, peers.begin() + i);
        }
        result->push_back(createApp(baseConfig, clock, quorumThresold, i, peers[i], myPeers));
    }

    return result;
}

struct AccountInfo {
    SecretKey key;
    uint64_t balance;
    uint32_t seq;
};
using accountPtr = shared_ptr<AccountInfo>;


accountPtr createRootAccount()
{
    return shared_ptr<AccountInfo>(new AccountInfo{ getRoot(), 0, 1 });
}
accountPtr createAccount(size_t i)
{
    auto accountName = "Account-" + to_string(i);
    return shared_ptr<AccountInfo>(new AccountInfo{ getAccount(accountName.c_str()), 0, 1 });
}

struct TxInfo {
    shared_ptr<AccountInfo> from;
    shared_ptr<AccountInfo> to;
    uint64_t amount;
    void execute(Application &app)
    {
        TransactionFramePtr txFrame = createPaymentTx(from->key, to->key, 1, amount);
        REQUIRE(app.getHerderGateway().recvTransaction(txFrame));

        from->seq++;
        from->balance -= amount;
        to->balance += amount;
    }
};

float rand_fraction()
{
    return  static_cast<float>(rand()) / static_cast<float>(RAND_MAX + 1);
}

float rand_pareto(float alpha)
{
    // from http://www.pamvotis.org/vassis/RandGen.htm
    return static_cast<float>(1) / static_cast<float>(pow(rand_fraction(), static_cast<float>(1) / alpha));
}


struct StressTest {
    shared_ptr<vector<appPtr>> apps;
    shared_ptr<vector<accountPtr>> accounts;
    size_t nAccounts;
    uint64_t initialFunds;

    void startApps()
    {
        for (auto app : *apps) {
            app->start();
        }
    }
    VirtualClock & getClock()
    {
        return (*apps)[0]->getClock();
    }
    TxInfo fundingTransaction(shared_ptr<AccountInfo> destination)
    {
        return TxInfo{ (*accounts)[0], destination, initialFunds };
    }
    TxInfo tranferTransaction(int iFrom, int iTo, uint64_t amount)
    {
        return TxInfo{ (*accounts)[iFrom], (*accounts)[iTo], amount };
    }
    TxInfo randomTransferTransaction(float alpha)
    {
        AccountInfo from, to;
        int iFrom, iTo;
        do
        {
            iFrom = static_cast<int>(rand_pareto(alpha) * accounts->size());
            iTo = static_cast<int>(rand_pareto(alpha) * accounts->size());
        } while (iFrom == iTo);

        uint64_t amount = static_cast<uint64_t>(rand_fraction() * (*accounts)[iFrom]->balance / 3);
        return tranferTransaction(iFrom, iTo, amount);
    }
    TxInfo randomTransaction(float alpha)
    {
        if (accounts->size() < nAccounts && rand_fraction() > 0.5)
        {
            auto newAcc = createAccount(accounts->size());
            accounts->push_back(newAcc);
            return fundingTransaction(newAcc);
        }
        else
        {
            return randomTransferTransaction(alpha);
        }
    }
    void crank(VirtualClock::duration d)
    {
        bool stop = false;
        VirtualTimer checkTimer(getClock());
        checkTimer.expires_from_now(d * apps->size());
        checkTimer.async_wait([&](const asio::error_code& error)
        {
            stop = true;
        });

        while (!stop)
        {
            int nIdle = 0;
            for (auto app : *apps)
            {
                if (stop)
                    return;

                if (app->crank(false) == 0)
                {
                    nIdle++;
                }
            }
            if (nIdle == apps->size())
                return;
        }
    }

};
TEST_CASE("stress", "[hrd-stress]")
{
    int nNodes = 4;
    int quorumThresold = 2;
    float paretoAlpha = 0.5;
    uint64_t initialFunds = 1000;
    size_t nAccounts = 100;
    size_t nTransactions = 1000000000;
    size_t injectionRate = 1000; // per sec

    VirtualClock clock;
    Config cfg(getTestConfig());
    cfg.RUN_STANDALONE = true;
    cfg.START_NEW_NETWORK = true;

    StressTest test {
        createApps(cfg, clock, nNodes, quorumThresold),
        shared_ptr<vector<accountPtr>>(),
        nAccounts,
        initialFunds
    };
    test.accounts->push_back(createRootAccount());
    test.startApps();

    size_t iTransactions = 0;
    auto begin = clock.now();
    while (iTransactions < nTransactions)
    {
        test.crank(chrono::seconds(1));

        auto elapsed = chrono::duration_cast<chrono::seconds>(clock.now() - begin);
        auto targetTxs = elapsed.count() * injectionRate;
        auto toInject = max(static_cast<size_t>(0), targetTxs - iTransactions);

        if (toInject == 0)
        {
            LOG(INFO) << "Not injecting; sleeping for 100ms";
            this_thread::sleep_for(chrono::milliseconds(100));
        } else
        {
            LOG(INFO) << "Injecting " << toInject << " transactions";

            for (int i = 0; i < toInject; i++)
            {
                test.randomTransaction(paretoAlpha).execute(*(*test.apps)[rand() % test.apps->size()]);
            }
        }
    }
}

