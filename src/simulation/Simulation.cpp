// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Simulation.h"

#include "main/test.h"
#include "util/Logging.h"
#include "util/types.h"
#include "ledger/LedgerMaster.h"
#include "overlay/PeerRecord.h"
#include "main/Application.h"
#include "overlay/PeerMaster.h"
#include "transactions/TxTests.h"
#include "herder/HerderGateway.h"
#include "medida/medida.h"
#include "util/Math.h"

namespace stellar
{

using namespace std;

uint64 
Simulation::getMinBalance()
{
    int64_t mx = 0;
    for (auto n : mNodes)
    {
        auto b = n.second->getLedgerMaster().getMinBalance(0);
        mx = (b > mx ? b : mx);
    }
    return mx;
}

Simulation::Simulation(Mode mode) :
    mMode(mode)
  , mConfigCount(0)
  , mIdleApp(Application::create(mClock, getTestConfig(++mConfigCount)))
{
}

Simulation::~Simulation()
{
    // tear down
    std::map<uint256, Application::pointer>::iterator it;
    for (it = mNodes.begin(); it != mNodes.end(); ++it) {
        it->second->getMainIOService().poll_one();
        it->second->getMainIOService().stop();
    }
}

VirtualClock& 
Simulation::getClock()
{
  return mClock;
}

uint256
Simulation::addNode(uint256 validationSeed, 
                    FBAQuorumSet qSet,
                    VirtualClock& clock)
{
    Config::pointer cfg = std::make_shared<Config>(getTestConfig(++mConfigCount));

    cfg->VALIDATION_KEY = SecretKey::fromSeed(validationSeed);
    cfg->QUORUM_THRESHOLD = qSet.threshold;
    cfg->START_NEW_NETWORK = true;
    cfg->RUN_STANDALONE = (mMode == OVER_LOOPBACK);

    for (auto q : qSet.validators)
    {
        cfg->QUORUM_SET.push_back(q);
    }

    Application::pointer result = Application::create(clock, *cfg);

    if (mMode == OVER_TCP) 
        result->enableRealTimer();

    uint256 nodeID = makePublicKey(validationSeed);
    mConfigs[nodeID] = cfg;
    mNodes[nodeID] = result;

    return nodeID;
}

Application::pointer
Simulation::getNode(uint256 nodeID)
{
    return mNodes[nodeID];
}
vector<Application::pointer> 
Simulation::getNodes()
{
    vector<Application::pointer> result;
    for (auto app : mNodes)
        result.push_back(app.second);
    return result;
}

void
Simulation::addConnection(uint256 initiator,
                          uint256 acceptor)
{
    if (mMode == OVER_LOOPBACK)
        addLoopbackConnection(initiator, acceptor);
    else addTCPConnection(initiator, acceptor);
}



std::shared_ptr<LoopbackPeerConnection>
Simulation::addLoopbackConnection(uint256 initiator, 
                          uint256 acceptor)
{
    std::shared_ptr<LoopbackPeerConnection> connection;
    if (mNodes[initiator] && mNodes[acceptor]) 
    {
        connection = std::make_shared<LoopbackPeerConnection>(
            *getNode(initiator), *getNode(acceptor));
        mConnections.emplace_back(connection);
    }
    return connection;
}

void
Simulation::addTCPConnection(uint256 initiator,
                             uint256 acceptor)
{
    if (mMode != OVER_TCP)
    {
        throw new runtime_error("Cannot add a TCP connection");
    }
    auto from = getNode(initiator);
    auto to = getNode(acceptor);
    PeerRecord pr{"127.0.0.1", to->getConfig().PEER_PORT, from->getClock().now(), 0, 10};
    from->getPeerMaster().connectTo(pr);
}

void 
Simulation::startAllNodes()
{
    // We wait for the connections to set up (HELLO).
    while(crankAllNodes() > 0);

    for(auto it : mNodes)
    {
        it.second->start();
    }
}

std::size_t
Simulation::crankNode(uint256 nodeID, int nbTicks)
{
    std::size_t count = 0;
    if (mNodes[nodeID])
    {
        for (int i = 0; i < nbTicks && nbTicks > 0; i ++)
            count += mNodes[nodeID]->crank(false);
    }
    return count;
}

std::size_t
Simulation::crankAllNodes(int nbTicks)
{
    std::size_t count = 0;
    for (int i = 0; i < nbTicks && nbTicks > 0; i ++)
    {
        std::map<uint256, Application::pointer>::iterator it;
        for (it = mNodes.begin(); it != mNodes.end(); ++it) {
            if (it->second->getMainIOService().stopped())
            {
                throw std::runtime_error("Simulation node shut down");
            }
            count += it->second->crank(false);
        }
    }
    return count;
}

bool Simulation::haveAllExternalized(int num)
{
    uint64_t min = INT_MAX;
    for(auto it = mNodes.begin(); it != mNodes.end(); ++it) 
    {
        auto n = it->second->getLedgerMaster().getLedgerNum();
        LOG(DEBUG) << "Ledger#: " << n;

        if (n < min)
            min = n;
    }
    return num <= min;
}

void
Simulation::crankForAtMost(VirtualClock::duration seconds)
{
    bool stop = false;
    auto stopIt = [&](const asio::error_code& error)
    {
        if (!error)
            stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop && crankAllNodes() > 0);

    if (stop)
        LOG(INFO) << "Simulation timed out";
    else LOG(INFO) << "Simulation complete";
}

void
Simulation::crankForAtLeast(VirtualClock::duration seconds)
{
    bool stop = false;
    auto stopIt = [&](const asio::error_code& error)
    {
        if (!error)
            stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop)
    {
        if (crankAllNodes() == 0)
            this_thread::sleep_for(chrono::milliseconds(50));
    }
}

void 
Simulation::crankUntil(function<bool()> const & predicate, VirtualClock::duration timeout)
{
    bool stop = false;
    auto stopIt = [&](const asio::error_code& error)
    {
        // TODO: the VirtualTimer triggers the timeout event even when
        //       additional events remain on the event loop.
        /*
        if (!error)
            stop = true;
        */
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(timeout);
    checkTimer.async_wait(stopIt);

    while (!stop)
    {
        if (crankAllNodes() == 0)
            this_thread::sleep_for(chrono::milliseconds(50));
        if (predicate())
            return;
    }
    throw new runtime_error("Simulation timed out");
}


Simulation::TxInfo
Simulation::createTranferTransaction(size_t iFrom, size_t iTo, uint64_t amount)
{
    return TxInfo{ mAccounts[iFrom], mAccounts[iTo], amount };
}

Simulation::TxInfo
Simulation::createRandomTransaction(float alpha)
{
    size_t iFrom, iTo;
    do
    {
        iFrom = rand_pareto(alpha, mAccounts.size());
        iTo = rand_pareto(alpha, mAccounts.size());
    } while (iFrom == iTo);

    uint64_t amount = static_cast<uint64_t>(rand_fraction() * min(static_cast<uint64_t>(1000), (mAccounts[iFrom]->mBalance - getMinBalance()) / 3));
    return createTranferTransaction(iFrom, iTo, amount);
}

void
Simulation::TxInfo::execute(shared_ptr<Application> app)
{
    mFrom->mSeq++;
    mFrom->mBalance -= mAmount;
    mFrom->mBalance -= app->getConfig().DESIRED_BASE_FEE;
    mTo->mBalance += mAmount;

    TransactionFramePtr txFrame = txtest::createPaymentTx(mFrom->mKey, mTo->mKey, mFrom->mSeq, mAmount);
    app->getHerderGateway().recvTransaction(txFrame);
}

vector<Simulation::TxInfo>
Simulation::createRandomTransactions(size_t n, float paretoAlpha)
{
    vector<TxInfo> result;
    for (int i = 0; i < n; i++)
    {
        result.push_back(createRandomTransaction(paretoAlpha));
    }
    return result;
}


vector<Simulation::TxInfo>
Simulation::createAccounts(int n)
{
    vector<TxInfo> result;
    if (mAccounts.empty())
    {
        auto root = make_shared<AccountInfo>(0, txtest::getRoot(), 1000000000, *this);
        mAccounts.push_back(root);
        result.push_back(root->creationTransaction());
    }

    for (int i = 0; i < n; i++)
    {
        auto accountName = "Account-" + to_string(mAccounts.size());
        auto account = make_shared<AccountInfo>(mAccounts.size(), txtest::getAccount(accountName.c_str()), 0, *this);
        mAccounts.push_back(account);
        result.push_back(account->creationTransaction());
    }
    return result;
}

Simulation::TxInfo 
Simulation::AccountInfo::creationTransaction()
{
    return TxInfo{ mSimulation.mAccounts[0], shared_from_this(), 100 * mSimulation.getMinBalance() + mSimulation.mAccounts.size() - 1 };
}

void 
Simulation::execute(TxInfo transaction)
{
    // Execute on the first node
    transaction.execute(mNodes.begin()->second);
}

void
Simulation::executeAll(vector<TxInfo> const& transactions)
{
    for (auto tx : transactions)
    {
        execute(tx);
    }
}

chrono::seconds
Simulation::executeStressTest(size_t nTransactions, int injectionRatePerSec, function<TxInfo(size_t)> generatorFn)
{
    size_t iTransactions = 0;
    auto startTime = chrono::system_clock::now();
    chrono::system_clock::duration signingTime(0);
    while (iTransactions < nTransactions)
    {
        auto elapsed = chrono::duration_cast<chrono::microseconds>(chrono::system_clock::now() - startTime);
        auto targetTxs = min(nTransactions, static_cast<size_t>(elapsed.count() * injectionRatePerSec / 1000000));

        if (iTransactions == targetTxs)
        {
            // When running on a real clock, this is a spin loop that waits for 
            // the next event to trigger, or for the next network message.
            //
            // When running on virtual time, this line is never hit unless the injection 
            // is below what the network can absorb, and there is nothing do to but
            // wait for the next injection.
            this_thread::sleep_for(chrono::milliseconds(50));
        }
        else {
            LOG(INFO) << "Injecting txs " << (targetTxs - iTransactions) << " transactions (" << iTransactions << "..." << targetTxs << " out of " << nTransactions << ")";
            auto tBegin = chrono::system_clock::now();

            for (; iTransactions < targetTxs; iTransactions++)
                execute(generatorFn(iTransactions));

            auto t = (chrono::system_clock::now() - tBegin);
            signingTime += t;
        }

        crankAllNodes(1);
    }

    LOG(INFO) << "executeStressTest signingTime: " << chrono::duration_cast<chrono::seconds>(signingTime).count();
    return chrono::duration_cast<chrono::seconds>(signingTime);
}

vector<Simulation::accountInfoPtr> 
Simulation::accountsOutOfSyncWithDb()
{
    vector<accountInfoPtr> result;
    int iApp = 0;
    int64_t totalOffsets = 0;
    for (auto pair : mNodes)
    {
        iApp++;
        auto app = pair.second; 
        for (auto accountIt = mAccounts.begin() + 1; accountIt != mAccounts.end(); accountIt++)
        {
            auto account = *accountIt;
            AccountFrame accountFrame;
            AccountFrame::loadAccount(account->mKey.getPublicKey(), accountFrame, app->getDatabase());
            int64_t offset = accountFrame.getBalance() - static_cast<int64_t>(account->mBalance);
            if (offset != 0)
            {
                LOG(DEBUG) << "On node " << iApp << ", account " << account->mId
                    << " is off by " << (offset)
                    << "\t(has " << accountFrame.getBalance() << " should have " << account->mBalance << ")";
                totalOffsets += abs(offset);
                result.push_back(account);
            }
        }
    }
    LOG(INFO) << "Ledger has not yet caught up to the simulation. totalOffsets: " << totalOffsets;
    return result;
}

string
Simulation::metricsSummary(string domain)
{
    auto& registry = getNodes().front()->getMetrics();
    auto const& metrics = registry.GetAllMetrics();
    std::stringstream out;

    medida::reporting::ConsoleReporter reporter{ registry, out };
    for (auto kv : metrics)
    {
        auto metric = kv.first;
        if (metric.domain() == domain)
        {
            out << "Metric " << metric.domain() << "." << metric.type() << "." << metric.name() << "\n";
            kv.second->Process(reporter);
        }
    }
    return out.str();
}

}
