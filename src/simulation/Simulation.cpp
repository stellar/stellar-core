// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Simulation.h"

#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerRecord.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/types.h"

#include "medida/medida.h"
#include "medida/reporting/console_reporter.h"

#include <thread>

namespace stellar
{

using namespace std;

Simulation::Simulation(Mode mode, Hash const& networkID,
                       std::function<Config()> confGen)
    : LoadGenerator(networkID)
    , mClock(mode == OVER_TCP ? VirtualClock::REAL_TIME
                              : VirtualClock::VIRTUAL_TIME)
    , mMode(mode)
    , mConfigCount(0)
    , mConfigGen(confGen)
{
    mIdleApp = Application::create(mClock, newConfig());
}

Simulation::~Simulation()
{

    // tear down
    mClock.getIOService().poll_one();
    mClock.getIOService().stop();
    while (mClock.cancelAllEvents())
        ;
}

VirtualClock&
Simulation::getClock()
{
    return mClock;
}

NodeID
Simulation::addNode(SecretKey nodeKey, SCPQuorumSet qSet, VirtualClock& clock,
                    Config const* cfg2, bool newDB )
{
    std::shared_ptr<Config> cfg;
    if (!cfg2)
    {
        cfg = std::make_shared<Config>(newConfig());
    }
    else
    {
        cfg = std::make_shared<Config>(*cfg2);
    }
    cfg->NODE_SEED = nodeKey;
    cfg->QUORUM_SET = qSet;
    cfg->RUN_STANDALONE = (mMode == OVER_LOOPBACK);

    Application::pointer result = Application::create(clock, *cfg, newDB);

    NodeID nodeID = nodeKey.getPublicKey();
    mConfigs[nodeID] = cfg;
    mNodes[nodeID] = result;

    return nodeID;
}

Application::pointer
Simulation::getNode(NodeID nodeID)
{
    return mNodes[nodeID];
}
vector<Application::pointer>
Simulation::getNodes()
{
    vector<Application::pointer> result;
    for (auto const& app : mNodes)
        result.push_back(app.second);
    return result;
}
vector<NodeID>
Simulation::getNodeIDs()
{
    vector<NodeID> result;
    for (auto const& app : mNodes)
        result.push_back(app.first);
    return result;
}

void
Simulation::addPendingConnection(NodeID const& initiator,
                                 NodeID const& acceptor)
{
    mPendingConnections.push_back(std::make_pair(initiator, acceptor));
}

void
Simulation::addConnection(NodeID initiator, NodeID acceptor)
{
    if (mMode == OVER_LOOPBACK)
        addLoopbackConnection(initiator, acceptor);
    else
        addTCPConnection(initiator, acceptor);
}

void
Simulation::addLoopbackConnection(NodeID initiator, NodeID acceptor)
{
    if (mNodes[initiator] && mNodes[acceptor])
    {
        auto conn = std::make_shared<LoopbackPeerConnection>(
            *getNode(initiator), *getNode(acceptor));
        mLoopbackConnections.push_back(conn);
    }
}

void
Simulation::addTCPConnection(NodeID initiator, NodeID acceptor)
{
    if (mMode != OVER_TCP)
    {
        throw runtime_error("Cannot add a TCP connection");
    }
    auto from = getNode(initiator);
    auto to = getNode(acceptor);
    PeerRecord pr{"127.0.0.1", to->getConfig().PEER_PORT,
                  from->getClock().now()};
    from->getOverlayManager().connectTo(pr);
}

void
Simulation::startAllNodes()
{
    for (auto const& it : mNodes)
    {
        it.second->start();
        updateMinBalance(*it.second);
    }

    for (auto const& pair : mPendingConnections)
    {
        addConnection(pair.first, pair.second);
    }
    mPendingConnections.clear();
}

void
Simulation::stopAllNodes()
{
    for (auto& n : mNodes)
    {
        n.second->gracefulStop();
    }

    while (crankAllNodes() > 0)
        ;
}

std::size_t
Simulation::crankAllNodes(int nbTicks)
{
    std::size_t count = 0;
    for (int i = 0; i < nbTicks && nbTicks > 0; i++)
    {
        if (mClock.getIOService().stopped())
        {
            return 0;
        }
        count += mClock.crank(false);
    }
    return count;
}

bool
Simulation::haveAllExternalized(SequenceNumber num, uint32 maxSpread)
{
    uint32_t min = UINT_MAX, max = 0;
    for (auto it = mNodes.begin(); it != mNodes.end(); ++it)
    {
        auto n = it->second->getLedgerManager().getLastClosedLedgerNum();
        LOG(DEBUG) << it->second->getConfig().PEER_PORT << " @ ledger#: " << n;

        if (n < min)
            min = n;
        if (n > max)
            max = n;
    }
    if (max - min > maxSpread)
    {
        throw std::runtime_error("Too wide spread between nodes");
    }
    return num <= min;
}

void
Simulation::crankForAtMost(VirtualClock::duration seconds, bool finalCrank)
{
    bool stop = false;
    auto stopIt = [&](asio::error_code const& error)
    {
        if (!error)
            stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop && crankAllNodes() > 0)
        ;

    if (stop)
        LOG(INFO) << "Simulation timed out";
    else
        LOG(INFO) << "Simulation complete";

    if (finalCrank)
    {
        stopAllNodes();
    }
}

void
Simulation::crankForAtLeast(VirtualClock::duration seconds, bool finalCrank)
{
    bool stop = false;
    auto stopIt = [&](asio::error_code const& error)
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
            std::this_thread::sleep_for(chrono::milliseconds(50));
    }

    if (finalCrank)
    {
        stopAllNodes();
    }
}

void
Simulation::crankUntilSync(VirtualClock::duration timeout, bool finalCrank)
{
    crankUntil(
        [&]()
        {
            return this->accountsOutOfSyncWithDb().empty();
        },
        timeout, finalCrank);
}

void
Simulation::crankUntil(function<bool()> const& predicate,
                       VirtualClock::duration timeout, bool finalCrank)
{
    bool timedOut = false;
    VirtualTimer timeoutTimer(*mIdleApp);
    timeoutTimer.expires_from_now(timeout);

    bool done = false;

    VirtualTimer checkTimer(*mIdleApp);
    function<void()> checkDone = [&]()
    {
        if (predicate())
            done = true;
        else
        {
            checkTimer.expires_from_now(chrono::seconds(1));
            checkTimer.async_wait(checkDone, &VirtualTimer::onFailureNoop);
        }
    };

    timeoutTimer.async_wait(
        [&]()
        {
            checkDone();
            timedOut = true;
        },
        &VirtualTimer::onFailureNoop);

    checkTimer.expires_from_now(chrono::seconds(1));
    checkTimer.async_wait(checkDone, &VirtualTimer::onFailureNoop);

    // initial check, pre crank (mostly used for getting a snapshot of the
    // starting state)
    checkDone();

    for (;;)
    {
        if (crankAllNodes() == 0)
        {
            checkDone();
            std::this_thread::sleep_for(chrono::milliseconds(1));
        }
        if (done)
        {
            if (finalCrank)
            {
                stopAllNodes();
            }
            return;
        }
        if (timedOut)
            throw runtime_error("Simulation timed out");
    }
}

void
Simulation::execute(TxInfo transaction)
{
    // Execute on the first node
    bool res = transaction.execute(*mNodes.begin()->second);
    if (!res)
    {
        CLOG(DEBUG, "Simulation") << "Failed execution in simulation";
    }
}

void
Simulation::executeAll(vector<TxInfo> const& transactions)
{
    for (auto& tx : transactions)
    {
        execute(tx);
    }
}

chrono::seconds
Simulation::executeStressTest(size_t nTransactions, int injectionRatePerSec,
                              function<TxInfo(size_t)> generatorFn)
{
    size_t iTransactions = 0;
    auto startTime = chrono::system_clock::now();
    chrono::system_clock::duration signingTime(0);
    while (iTransactions < nTransactions)
    {
        auto elapsed = chrono::duration_cast<chrono::microseconds>(
            chrono::system_clock::now() - startTime);
        auto targetTxs = min(
            nTransactions, static_cast<size_t>(elapsed.count() *
                                               injectionRatePerSec / 1000000));

        if (iTransactions == targetTxs)
        {
            // When running on a real clock, this is a spin loop that waits for
            // the next event to trigger, or for the next network message.
            //
            // When running on virtual time, this line is never hit unless the
            // injection is below what the network can absorb, and there is
            // nothing do to but wait for the next injection.
            std::this_thread::sleep_for(chrono::milliseconds(50));
        }
        else
        {
            LOG(INFO) << "Injecting txs " << (targetTxs - iTransactions)
                      << " transactions (" << iTransactions << "..."
                      << targetTxs << " out of " << nTransactions << ")";
            auto tBegin = chrono::system_clock::now();

            for (; iTransactions < targetTxs; iTransactions++)
                execute(generatorFn(iTransactions));

            auto t = (chrono::system_clock::now() - tBegin);
            signingTime += t;
        }

        crankAllNodes(1);
    }

    LOG(INFO) << "executeStressTest signingTime: "
              << chrono::duration_cast<chrono::seconds>(signingTime).count();
    return chrono::duration_cast<chrono::seconds>(signingTime);
}

vector<Simulation::AccountInfoPtr>
Simulation::accountsOutOfSyncWithDb()
{
    vector<AccountInfoPtr> result;
    int iApp = 0;
    int64_t totalOffsets = 0;
    for (auto const& pair : mNodes)
    {
        iApp++;
        auto app = pair.second;
        for (auto accountIt = mAccounts.begin() + 1;
             accountIt != mAccounts.end(); accountIt++)
        {
            auto account = *accountIt;
            AccountFrame::pointer accountFrame;
            accountFrame = AccountFrame::loadAccount(
                account->mKey.getPublicKey(), app->getDatabase());
            int64_t offset;
            if (accountFrame)
            {
                offset = accountFrame->getBalance() -
                         static_cast<int64_t>(account->mBalance);
                account->mSeq = accountFrame->getSeqNum();
            }
            else
            {
                offset = -1;
            }
            if (offset != 0)
            {
                LOG(DEBUG) << "On node " << iApp << ", account " << account->mId
                           << " is off by " << (offset) << "\t(has "
                           << (accountFrame ? accountFrame->getBalance() : 0)
                           << " should have " << account->mBalance << ")";
                totalOffsets += abs(offset);
                result.push_back(account);
            }
        }
    }
    LOG(INFO)
        << "Ledger has not yet caught up to the simulation. totalOffsets: "
        << totalOffsets;
    return result;
}

bool
Simulation::loadAccount(AccountInfo& account)
{
    // assumes all nodes are in sync
    auto app = mNodes.begin()->second;
    return LoadGenerator::loadAccount(*app, account);
}

Config
Simulation::newConfig()
{
    if (mConfigGen)
    {
        return mConfigGen();
    }
    else
    {
        Config res = getTestConfig(mConfigCount++);
        res.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        return res;
    }
}

class ConsoleReporterWithSum : public medida::reporting::ConsoleReporter
{
    std::ostream& out_;

  public:
    ConsoleReporterWithSum(medida::MetricsRegistry& registry,
                           std::ostream& out = std::cerr)
        : medida::reporting::ConsoleReporter(registry, out), out_(out)
    {
    }

    void
    Process(medida::Timer& timer) override
    {
        auto snapshot = timer.GetSnapshot();
        auto unit = "ms";
        out_ << "           count = " << timer.count() << endl
             << "             sum = " << timer.count() * timer.mean() << unit
             << endl
             << "             min = " << timer.min() << unit << endl
             << "             max = " << timer.max() << unit << endl
             << "            mean = " << timer.mean() << unit << endl
             << "          stddev = " << timer.std_dev() << unit << endl;
    }
};

string
Simulation::metricsSummary(string domain)
{
    auto& registry = getNodes().front()->getMetrics();
    auto const& metrics = registry.GetAllMetrics();
    std::stringstream out;

    ConsoleReporterWithSum reporter{registry, out};
    for (auto const& kv : metrics)
    {
        auto metric = kv.first;
        if (domain == "" || metric.domain() == domain)
        {
            out << "Metric " << metric.domain() << "." << metric.type() << "."
                << metric.name() << "\n";
            kv.second->Process(reporter);
        }
    }
    return out.str();
}
}
