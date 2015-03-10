
#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Config.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "generated/StellarXDR.h"
#include "util/Timer.h"
#include "crypto/SHA.h"
#include "medida/medida.h"

#define SIMULATION_CREATE_NODE(N) \
    const Hash v##N##VSeed = sha256("SEED_VALIDATION_SEED_" #N);    \
    const SecretKey v##N##SecretKey = SecretKey::fromSeed(v##N##VSeed); \
    const Hash v##N##NodeID = v##N##SecretKey.getPublicKey();

namespace stellar
{
using namespace std;
    
class Simulation
{
  public:
      enum Mode
      {
          OVER_TCP,
          OVER_LOOPBACK
      };

    typedef shared_ptr<Simulation> pointer;

    Simulation(Mode mode);
    ~Simulation();

    VirtualClock& getClock();

    uint256 addNode(uint256 validationSeed, 
                    SCPQuorumSet qSet,
                    VirtualClock& clock);
    Application::pointer getNode(uint256 nodeID);
    vector<Application::pointer> getNodes();

    void
        addConnection(uint256 initiator,
        uint256 acceptor);
        
    shared_ptr<LoopbackPeerConnection>
        addLoopbackConnection(uint256 initiator, 
                      uint256 acceptor);

    void addTCPConnection(uint256 initiator,
                          uint256 acception);
        
    void startAllNodes();

    bool haveAllExternalized(int num);

    size_t crankAllNodes(int nbTicks=1);
    void crankForAtMost(VirtualClock::duration seconds);
    void crankForAtLeast(VirtualClock::duration seconds);
    void crankUntil(function<bool()> const& fn, VirtualClock::duration timeout);

    //////////

    struct TxInfo;

    class AccountInfo : public enable_shared_from_this<AccountInfo> {
    public:
        AccountInfo(size_t id, SecretKey key, uint64_t balance, Simulation & simulation) : mId(id), mKey(key), mBalance(balance), mSeq(0), mSimulation(simulation) {}
        size_t mId;
        SecretKey mKey;
        uint64_t mBalance;
        SequenceNumber mSeq;

        TxInfo creationTransaction();
    private:
        Simulation& mSimulation;
    };
    using accountInfoPtr = shared_ptr<AccountInfo>;
    vector<accountInfoPtr> mAccounts;

    struct TxInfo {
        accountInfoPtr mFrom;
        accountInfoPtr mTo;
        uint64_t mAmount;
        void execute(shared_ptr<Application> app);
    };



    vector<Simulation::TxInfo> createAccounts(int n);
    TxInfo createTranferTransaction(size_t iFrom, size_t iTo, uint64_t amount);
    TxInfo createRandomTransaction(float alpha);
    vector<Simulation::TxInfo> createRandomTransactions(size_t n, float paretoAlpha);

    void execute(TxInfo transaction);
    void executeAll(vector<TxInfo> const& transaction);
    chrono::seconds executeStressTest(size_t nTransactions, int injectionRatePerSec, function<TxInfo(size_t)> generatorFn);

    vector<accountInfoPtr> accountsOutOfSyncWithDb(); // returns the accounts that don't match
    void SyncSequenceNumbers();

    string metricsSummary(string domain);

private:
    VirtualClock mClock;
    Mode mMode;
    int mConfigCount;
    Application::pointer mIdleApp;
    map<uint256, Config::pointer> mConfigs;
    map<uint256, Application::pointer> mNodes;
    vector<shared_ptr<LoopbackPeerConnection>> mConnections;

    uint64 getMinBalance();
};
}


