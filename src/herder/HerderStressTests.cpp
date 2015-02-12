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
#include "util/Math.h"
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

    auto result = make_shared<Application>(clock, cfg);
    result->enableRealTimer();
    return result;
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
        if (n == 1)
        {
            // no peers
        }
        else if (i < quorumThresold)
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
    size_t id;
    SecretKey key;
    uint64_t balance;
    uint32_t seq;
    chrono::time_point<chrono::system_clock> creationTime;

    bool isCreated()
    {
        return (chrono::system_clock::now() - creationTime) > chrono::seconds(100);
    }

};
using accountPtr = shared_ptr<AccountInfo>;


accountPtr createRootAccount()
{
    return shared_ptr<AccountInfo>(new AccountInfo{ 0, getRoot(), 1000000000, 1, chrono::system_clock::now() - chrono::seconds(100) });
}
accountPtr createAccount(size_t i)
{
    auto accountName = "Account-" + to_string(i);
    return shared_ptr<AccountInfo>(new AccountInfo{ i, getAccount(accountName.c_str()), 0, 1, chrono::system_clock::now() });
}

struct TxInfo {
    shared_ptr<AccountInfo> from;
    shared_ptr<AccountInfo> to;
    uint64_t amount;
    void execute(shared_ptr<Application> app)
    {
        TransactionFramePtr txFrame = createPaymentTx(from->key, to->key, 1, amount);
        REQUIRE(app->getHerderGateway().recvTransaction(txFrame));

        from->seq++;
        from->balance -= amount;
        to->balance += amount;
    }
    bool bothCreated()
    {
        return from->id == 0 || (from->isCreated() && to->isCreated());
    }
};


struct StressTest {
    shared_ptr<vector<appPtr>> apps;
    vector<accountPtr> accounts;
    size_t nAccounts;
    uint64_t minBalance;

    void startApps()
    {
        for (auto app : *apps) 
        {
            app->start();
            AccountFrame rootAccount;
            REQUIRE(AccountFrame::loadAccount(
                accounts[0]->key.getPublicKey(), rootAccount, app->getDatabase()));
        }
        minBalance = (*apps)[0]->getLedgerMaster().getMinBalance(0);
    }
    VirtualClock & getClock()
    {
        return (*apps)[0]->getClock();
    }
    TxInfo fundingTransaction(shared_ptr<AccountInfo> destination)
    {
        return TxInfo{ accounts[0], destination, 100*minBalance };
    }
    TxInfo tranferTransaction(size_t iFrom, size_t iTo, uint64_t amount)
    {
        return TxInfo{ accounts[iFrom], accounts[iTo], amount };
    }
    TxInfo randomTransferTransaction(float alpha)
    {
        AccountInfo from, to;
        size_t iFrom, iTo;
        do
        {
            iFrom = rand_pareto(alpha, accounts.size());
            iTo = rand_pareto(alpha, accounts.size());
        } while (iFrom == iTo);

        uint64_t amount = static_cast<uint64_t>(rand_fraction() * min(static_cast<uint64_t>(1000), (accounts[iFrom]->balance - minBalance) / 3));
        return tranferTransaction(iFrom, iTo, amount);
    }
    TxInfo randomTransaction(float alpha)
    {
        if (accounts.size() < nAccounts && (accounts.size() < 4 || rand_fraction() > 0.5))
        {
            auto newAcc = createAccount(accounts.size());
            accounts.push_back(newAcc);
            return fundingTransaction(newAcc);
        }
        else
        {
            return randomTransferTransaction(alpha);
        }
    }
    void injectRandomTransactions(size_t n, float paretoAlpha)
    {
        for (int i = 0; i < n; )
        {
            auto tx = randomTransaction(paretoAlpha);
            if (tx.bothCreated())
            {
                LOG(INFO) << "tx " << tx.from->id << " " << tx.to->id << "  $" << tx.amount;
                tx.execute((*apps)[rand() % apps->size()]);
                i++;
            }
        }
    }
    void crank(chrono::seconds t)
    {
        bool stop = false;

        auto begin = chrono::system_clock::now();
        while (chrono::system_clock::now() - begin < t)
        {
            auto nIdle = 0;
            for (int i = 0; i < apps->size(); i++)
            {
                if (stop)
                    return;

                if ((*apps)[i]->crank(false) == 0)
                {
                    LOG(INFO) << "cranked " << i << "; now idle";
                    nIdle++;
                }
                else
                    LOG(INFO) << "cranked " << i;
            }
            if (nIdle == apps->size()) {
                LOG(INFO) << "all idle; sleeping for 100ms";
                this_thread::sleep_for(chrono::milliseconds(100));
            }
        }
    }

};
TEST_CASE("stress", "[hrd-stress]")
{
    int nNodes = 1;
    int quorumThresold = 1;
    float paretoAlpha = 0.5;
    uint64_t initialFunds = 1000;
    size_t nAccounts = 100;
    size_t nTransactions = 1000000000;
    size_t injectionRate = 1; // per sec

    VirtualClock clock;
    Config cfg(getTestConfig());
    cfg.RUN_STANDALONE = true;
    cfg.START_NEW_NETWORK = true;


    StressTest test {
        createApps(cfg, clock, nNodes, quorumThresold),
        vector<accountPtr>(),
        nAccounts,
    };
    test.accounts.push_back(createRootAccount());
    test.startApps();



    size_t iTransactions = 0;
    auto begin = chrono::system_clock::now();
    while (iTransactions < nTransactions)
    {
        auto elapsed = chrono::duration_cast<chrono::microseconds>(chrono::system_clock::now() - begin);
        auto targetTxs = elapsed.count() * injectionRate / 1000000;
        auto toInject = max(static_cast<size_t>(0), targetTxs - iTransactions);

        if (toInject == 0)
        {
            LOG(INFO) << "Not injecting; sleeping for 100ms";
            this_thread::sleep_for(chrono::milliseconds(100));
        } else
        {
            LOG(INFO) << "Injecting " << toInject << " transactions";
            test.injectRandomTransactions(toInject, paretoAlpha);
            iTransactions += toInject;
        }

        test.crank(chrono::seconds(1));
    }
}

