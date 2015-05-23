
#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "generated/StellarXDR.h"
#include "util/Timer.h"
#include "crypto/SHA.h"
#include "medida/medida.h"
#include "transactions/TxTests.h"
#include "generated/Stellar-types.h"

#define SIMULATION_CREATE_NODE(N)                                              \
    const Hash v##N##VSeed = sha256("SEED_VALIDATION_SEED_" #N);               \
    const SecretKey v##N##SecretKey = SecretKey::fromSeed(v##N##VSeed);        \
    const Hash v##N##NodeID = v##N##SecretKey.getPublicKey();

namespace stellar
{

class Simulation
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

    uint256 addNode(SecretKey nodeKey, SCPQuorumSet qSet, VirtualClock& clock,
                    Config::pointer cfg = std::shared_ptr<Config>());
    Application::pointer getNode(uint256 nodeID);
    std::vector<Application::pointer> getNodes();
    std::vector<uint256> getNodeIDs();

    void addConnection(uint256 initiator, uint256 acceptor);

    std::shared_ptr<LoopbackPeerConnection>
    addLoopbackConnection(uint256 initiator, uint256 acceptor);

    void addTCPConnection(uint256 initiator, uint256 acception);

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

    struct TxInfo;

    class AccountInfo : public std::enable_shared_from_this<AccountInfo>
    {
      public:
        AccountInfo(Simulation& simulation) : mSimulation(simulation)
        {
        }
        AccountInfo(size_t id, SecretKey key, uint64_t balance,
                    SequenceNumber seq, Simulation& simulation)
            : mId(id)
            , mKey(key)
            , mBalance(balance)
            , mSeq(seq)
            , mSimulation(simulation)
        {
        }
        size_t mId;
        SecretKey mKey;
        uint64_t mBalance;
        SequenceNumber mSeq;

        TxInfo creationTransaction();

      private:
        Simulation& mSimulation;
    };
    using AccountInfoPtr = std::shared_ptr<AccountInfo>;
    std::vector<AccountInfoPtr> mAccounts;

    struct TxInfo
    {
        AccountInfoPtr mFrom;
        AccountInfoPtr mTo;
        bool mCreate;
        uint64_t mAmount;
        void execute(Application& app);
        TransactionFramePtr createPaymentTx();
        void recordExecution(uint64_t baseFee);
    };

    std::vector<Simulation::TxInfo> accountCreationTransactions(size_t n);
    Simulation::AccountInfoPtr createAccount(size_t i);
    std::vector<Simulation::AccountInfoPtr> createAccounts(size_t n);
    TxInfo createTransferTransaction(size_t iFrom, size_t iTo, uint64_t amount);
    TxInfo createRandomTransaction(float alpha);
    std::vector<Simulation::TxInfo> createRandomTransactions(size_t n,
                                                             float paretoAlpha);

    void execute(TxInfo transaction);
    void executeAll(std::vector<TxInfo> const& transaction);
    std::chrono::seconds
    executeStressTest(size_t nTransactions, int injectionRatePerSec,
                      std::function<TxInfo(size_t)> generatorFn);

    std::vector<AccountInfoPtr>
    accountsOutOfSyncWithDb(); // returns the accounts that don't match
    bool loadAccount(AccountInfo& account);
    void loadAccounts();

    std::string metricsSummary(std::string domain = "");

  private:
    VirtualClock mClock;
    Mode mMode;
    int mConfigCount;
    Application::pointer mIdleApp;
    std::map<uint256, Config::pointer> mConfigs;
    std::map<uint256, Application::pointer> mNodes;
    std::vector<std::shared_ptr<LoopbackPeerConnection>> mConnections;

  protected:
    uint64 getMinBalance();
};
}
