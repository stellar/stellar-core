
#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "main/StellarXDR.h"
#include "util/Timer.h"
#include "crypto/SHA.h"
#include "medida/medida.h"
#include "transactions/TxTests.h"
#include "main/StellarXDR.h"
#include "simulation/LoadGenerator.h"

#define SIMULATION_CREATE_NODE(N)                                              \
    const Hash v##N##VSeed = sha256("SEED_VALIDATION_SEED_" #N);               \
    const SecretKey v##N##SecretKey = SecretKey::fromSeed(v##N##VSeed);        \
    const PublicKey v##N##NodeID = v##N##SecretKey.getPublicKey();

namespace stellar
{
using xdr::operator<;
using xdr::operator==;

class Simulation : public LoadGenerator
{
  public:
    enum Mode
    {
        OVER_TCP,
        OVER_LOOPBACK
    };

    typedef std::shared_ptr<Simulation> pointer;

    Simulation(Mode mode);
    ~Simulation();

    VirtualClock& getClock();

    NodeID addNode(SecretKey nodeKey, SCPQuorumSet qSet, VirtualClock& clock,
                   Config::pointer cfg = std::shared_ptr<Config>());
    Application::pointer getNode(NodeID nodeID);
    std::vector<Application::pointer> getNodes();
    std::vector<NodeID> getNodeIDs();

    void addConnection(NodeID initiator, NodeID acceptor);

    std::shared_ptr<LoopbackPeerConnection>
    addLoopbackConnection(NodeID initiator, NodeID acceptor);

    void addTCPConnection(NodeID initiator, NodeID acception);

    void startAllNodes();
    void stopAllNodes();

    bool haveAllExternalized(SequenceNumber num);

    size_t crankAllNodes(int nbTicks = 1);
    void crankForAtMost(VirtualClock::duration seconds, bool finalCrank);
    void crankForAtLeast(VirtualClock::duration seconds, bool finalCrank);
    void crankUntilSync(VirtualClock::duration timeout, bool finalCrank);
    void crankUntil(std::function<bool()> const& fn,
                    VirtualClock::duration timeout, bool finalCrank);

    //////////

    void execute(TxInfo transaction);
    void executeAll(std::vector<TxInfo> const& transaction);
    std::chrono::seconds
    executeStressTest(size_t nTransactions, int injectionRatePerSec,
                      std::function<TxInfo(size_t)> generatorFn);

    std::vector<AccountInfoPtr>
    accountsOutOfSyncWithDb(); // returns the accounts that don't match
    bool loadAccount(AccountInfo& account);
    std::string metricsSummary(std::string domain = "");

  private:
    VirtualClock mClock;
    Mode mMode;
    int mConfigCount;
    Application::pointer mIdleApp;
    std::map<NodeID, Config::pointer> mConfigs;
    std::map<NodeID, Application::pointer> mNodes;
    std::vector<std::shared_ptr<LoopbackPeerConnection>> mConnections;
};
}
