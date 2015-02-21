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
#include <thread>
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
#include "util/TmpDir.h"

using namespace stellar;
using namespace stellar::txtest;
using namespace std;

namespace stellar 
{
using appPtr = shared_ptr<Application>;

struct PeerInfo {
    SecretKey peerKey;
    SecretKey validationKey;
    int peerPort;
};

appPtr
createApp(VirtualClock &clock, int quorumThresold, int i, PeerInfo &me, vector<PeerInfo> &peers) 
{
    Config cfg(getTestConfig(i));
    cfg.RUN_STANDALONE = false;
    cfg.PEER_KEY = me.peerKey;
    cfg.PEER_PUBLIC_KEY = me.peerKey.getPublicKey();
    cfg.VALIDATION_KEY = me.validationKey;
    cfg.PEER_PORT = me.peerPort;
    cfg.HTTP_PORT = me.peerPort + 1;

    cfg.DATABASE = "sqlite3://" + cfg.TMP_DIR_PATH + "/stellar.db";

    cfg.QUORUM_THRESHOLD = min(quorumThresold / 2 + 4, quorumThresold);
    cfg.PREFERRED_PEERS.clear();
    cfg.QUORUM_SET.clear();
    cfg.QUORUM_SET.push_back(me.validationKey.getPublicKey());
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
createApps(VirtualClock &clock, int n, int quorumThresold) 
{
    vector<PeerInfo> peers;

    for (int i = 0; i < n; i++) 
    {
        peers.push_back(PeerInfo { SecretKey::random(), SecretKey::random(), getTestConfig(i).PEER_PORT });
    }

    auto result = make_shared<vector<appPtr>>();

    for (int i = 0; i < n; i++)
    {
        vector<PeerInfo> myPeers;
        if (n < 2 * quorumThresold)
        {
            // Full connectivity.
            myPeers = peers;
            myPeers.erase(myPeers.begin() + i);
        }
        else if (i < quorumThresold)
        {
            // The first few nodes depend on the next `quorumThresold` ones.
            myPeers = vector<PeerInfo>(peers.begin() + i + 1, peers.begin() + i + 1 + quorumThresold);
        }
        else {
            // The other nodes depend on the `quorumThresold` previous ones.
            myPeers = vector<PeerInfo>(peers.begin() + i - quorumThresold, peers.begin() + i);
        }
        result->push_back(createApp(clock, quorumThresold, i, peers[i], myPeers));
    }

    return result;
}

struct AccountInfo {
    size_t mId;
    SecretKey mKey;
    uint64_t mBalance;
    uint32_t mSeq;
};
using accountPtr = shared_ptr<AccountInfo>;


accountPtr createRootAccount()
{
    return shared_ptr<AccountInfo>(new AccountInfo{ 0, getRoot(), 1000000000, 1 });
}
accountPtr createAccount(size_t i)
{
    auto accountName = "Account-" + to_string(i);
    return shared_ptr<AccountInfo>(new AccountInfo{ i, getAccount(accountName.c_str()), 0, 1 });
}

struct TxInfo {
    shared_ptr<AccountInfo> mFrom;
    shared_ptr<AccountInfo> mTo;
    uint64_t mAmount;
    void execute(shared_ptr<Application> app)
    {
        TransactionFramePtr txFrame = createPaymentTx(mFrom->mKey, mTo->mKey, mFrom->mSeq, mAmount);
        REQUIRE(app->getHerderGateway().recvTransaction(txFrame));

        mFrom->mSeq++;
        mFrom->mBalance -= mAmount;
        mFrom->mBalance -= app->getConfig().DESIRED_BASE_FEE;
        mTo->mBalance += mAmount;
    }
};


struct StressTest {
    shared_ptr<vector<appPtr>> mApps;
    vector<accountPtr> mAccounts;
    size_t mNAccounts;
    uint64_t mMinBalance;

    void startApps()
    {
        for (auto app : *mApps) 
        {
            app->start();
            AccountFrame rootAccount;
            REQUIRE(AccountFrame::loadAccount(
                mAccounts[0]->mKey.getPublicKey(), rootAccount, app->getDatabase()));
        }
        mMinBalance = (*mApps)[0]->getLedgerMaster().getMinBalance(0);
    }
    VirtualClock & getClock()
    {
        return (*mApps)[0]->getClock();
    }
    TxInfo accountCreationTransaction()
    {
        auto newAcc = createAccount(mAccounts.size());
        mAccounts.push_back(newAcc);
        return TxInfo{ mAccounts[0], newAcc, 100 * mMinBalance + mAccounts.size() - 1 };
    }
    TxInfo tranferTransaction(size_t iFrom, size_t iTo, uint64_t amount)
    {
        return TxInfo{ mAccounts[iFrom], mAccounts[iTo], amount };
    }
    TxInfo randomTransaction(float alpha)
    {
        AccountInfo from, to;
        size_t iFrom, iTo;
        do
        {
            iFrom = rand_pareto(alpha, mAccounts.size());
            iTo = rand_pareto(alpha, mAccounts.size());
        } while (iFrom == iTo);

        uint64_t amount = static_cast<uint64_t>(rand_fraction() * min(static_cast<uint64_t>(1000), (mAccounts[iFrom]->mBalance - mMinBalance) / 3));
        return tranferTransaction(iFrom, iTo, amount);
    }
    void injectTransaction(TxInfo tx)
    {
        //LOG(INFO) << "tx " << tx.mFrom->mId << " " << tx.mTo->mId << "  $" << tx.mAmount;
        tx.execute((*mApps)[rand() % mApps->size()]);
    }
    void injectRandomTransactions(size_t n, float paretoAlpha)
    {
        LOG(INFO) << "Injecting " << n << " transactions";
        for (int i = 0; i < n; i++)
        {
            injectTransaction(randomTransaction(paretoAlpha));
        }
    }
    void crankAll()
    {
        for (auto app : *mApps)
        {
            while (app->crank(false) > 0);
        }
    }
    void crank(chrono::seconds atMost)
    {
        auto begin = chrono::system_clock::now();

        while(true)
        {
            auto nIdle = 0;

            for (int i = 0; i < mApps->size(); i++)
            {
                if ((*mApps)[i]->crank(false) == 0)
                {
                    nIdle++;
                }
                if (chrono::system_clock::now() - begin > atMost)
                {
                    //return;
                }
            }
            if (nIdle == mApps->size()) {
                return;
            }
        }
    }
    void check()
    {
        for (auto app : *mApps)
        {
            for (auto account = mAccounts.begin() + 1; account != mAccounts.end(); account++)
            {
                check(*app, **account);
            }
        }
    }
    void check(Application& app, AccountInfo& account)
    {
        AccountFrame accountFrame;
        AccountFrame::loadAccount(account.mKey.getPublicKey(), accountFrame, app.getDatabase());

        REQUIRE(accountFrame.getBalance() == account.mBalance);
    }

};

void herderStressTest(int nNodes, int quorumThresold, size_t nAccounts, size_t nTransactions, size_t injectionRate, float paretoAlpha)
{
    VirtualClock clock;


    StressTest test{
        createApps(clock, nNodes, quorumThresold),
        vector<accountPtr>(),
        nAccounts,
    };
    test.mAccounts.push_back(createRootAccount());
    test.startApps();

    // Dodge the bug in VirtualTime's implementation of syncing with the real clock
    for (auto app : (*test.mApps))
    {
        app->getMainIOService().post([]() { return; });
    }

    LOG(INFO) << "Creating " << nAccounts << " accounts";
    for (int i = 0; i < nAccounts; i++)
    {
        test.injectTransaction(test.accountCreationTransaction());
    }
    this_thread::sleep_for(chrono::seconds(10));
    test.crankAll();


    size_t iTransactions = 0;
    auto begin = chrono::system_clock::now();
    while (iTransactions < nTransactions)
    {
        auto elapsed = chrono::duration_cast<chrono::microseconds>(chrono::system_clock::now() - begin);
        auto targetTxs = min(nTransactions, static_cast<size_t>(elapsed.count() * injectionRate / 1000000));
        auto toInject = max(static_cast<size_t>(0), targetTxs - iTransactions);

        if (toInject == 0)
        {
            this_thread::sleep_for(chrono::milliseconds(50));
        }
        else
        {
            test.injectRandomTransactions(toInject, paretoAlpha);
            iTransactions += toInject;
        }

        test.crank(chrono::seconds(1));
    }
    auto endTime = chrono::seconds(10);
    this_thread::sleep_for(endTime);
    test.crankAll();

    test.check();

    auto secs = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now() - begin).count() - endTime.count();
    LOG(INFO) << "all done (" << static_cast<float>(nTransactions) / secs << " tx/sec)";
}

TEST_CASE("Randomised test of Herder, 50 accounts, 40 transactions", "[hrd-random]")
{
    int nNodes = 1;
    int quorumThresold = 1;
    float paretoAlpha = 0.5;

    size_t nAccounts = 50;
    size_t nTransactions = 40;
    size_t injectionRate = 4; // per sec

    return herderStressTest(nNodes, quorumThresold, nAccounts, nTransactions, injectionRate, paretoAlpha);
}

TEST_CASE("Stress test of Herder, 1000 accounts, 100k transactions", "[hrd-stress][hide]")
{
    int nNodes = 1;
    int quorumThresold = 1;
    float paretoAlpha = 0.5;

    size_t nAccounts = 1000;
    size_t nTransactions = 100000;
    size_t injectionRate = 300; // per sec

    return herderStressTest(nNodes, quorumThresold, nAccounts, nTransactions, injectionRate, paretoAlpha);
}


}