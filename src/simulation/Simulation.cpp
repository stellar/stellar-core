// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Simulation.h"

#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerManager.h"
#include "scp/LocalNode.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/finally.h"
#include "util/types.h"

#include <fmt/format.h>

#include "main/ApplicationUtils.h"
#include "medida/medida.h"
#include "medida/reporting/console_reporter.h"

#include <thread>

namespace stellar
{

using namespace std;

Simulation::Simulation(Mode mode, Hash const& networkID, ConfigGen confGen,
                       QuorumSetAdjuster qSetAdjust)
    : mVirtualClockMode(mode != OVER_TCP)
    , mClock(mVirtualClockMode ? VirtualClock::VIRTUAL_TIME
                               : VirtualClock::REAL_TIME)
    , mMode(mode)
    , mConfigCount(0)
    , mConfigGen(confGen)
    , mQuorumSetAdjuster(qSetAdjust)
{
    auto cfg = newConfig();
    auto& parallel = cfg.BACKGROUND_OVERLAY_PROCESSING;
    parallel = parallel && mVirtualClockMode == VirtualClock::REAL_TIME;
    mIdleApp = Application::create(mClock, cfg);
    mPeerMap.emplace(mIdleApp->getConfig().PEER_PORT, mIdleApp);
}

Simulation::~Simulation()
{
    // kills all connections
    mLoopbackConnections.clear();

    // destroy all nodes first
    mNodes.clear();

    // kill scheduler before the io service
    testutil::shutdownWorkScheduler(*mIdleApp);

    // shutdown overlay service such that it doesn't post anything to
    // soon-to-be-dead main io service killed right below
    mIdleApp->getOverlayManager().shutdown();

    // tear down main app/clock
    mClock.getIOContext().poll_one();
    mClock.getIOContext().stop();
    while (mClock.cancelAllEvents())
        ;
}

void
Simulation::setCurrentVirtualTime(VirtualClock::time_point t)
{
    mClock.setCurrentVirtualTime(t);
    for (auto& p : mNodes)
    {
        p.second.mClock->setCurrentVirtualTime(t);
    }
}

void
Simulation::setCurrentVirtualTime(VirtualClock::system_time_point t)
{
    mClock.setCurrentVirtualTime(t);
    for (auto& p : mNodes)
    {
        p.second.mClock->setCurrentVirtualTime(t);
    }
}

Application::pointer
Simulation::addNode(SecretKey nodeKey, SCPQuorumSet qSet, Config const* cfg2,
                    bool newDB)
{
    auto cfg = cfg2 ? std::make_shared<Config>(*cfg2)
                    : std::make_shared<Config>(newConfig());
    cfg->adjust();
    cfg->NODE_SEED = nodeKey;
    cfg->MANUAL_CLOSE = false;
    auto& parallel = cfg->BACKGROUND_OVERLAY_PROCESSING;
    parallel = parallel && mVirtualClockMode == VirtualClock::REAL_TIME;

    if (mQuorumSetAdjuster)
    {
        cfg->QUORUM_SET = mQuorumSetAdjuster(qSet);
    }
    else
    {
        cfg->QUORUM_SET = qSet;
    }

    if (mMode == OVER_TCP)
    {
        cfg->RUN_STANDALONE = false;
    }

    auto clock =
        make_shared<VirtualClock>(mVirtualClockMode ? VirtualClock::VIRTUAL_TIME
                                                    : VirtualClock::REAL_TIME);
    if (mVirtualClockMode)
    {
        clock->setCurrentVirtualTime(mClock.now());
    }

    Application::pointer app;
    if (mMode == OVER_LOOPBACK)
    {
        app = createTestApplication<ApplicationLoopbackOverlay, Simulation&>(
            *clock, *cfg, *this, newDB, false);
    }
    else
    {
        app = createTestApplication(*clock, *cfg, newDB, false);
    }

    mNodes.emplace(nodeKey.getPublicKey(), Node{clock, app});

    mPeerMap.emplace(app->getConfig().PEER_PORT,
                     std::weak_ptr<Application>(app));
    return app;
}

Application::pointer
Simulation::getNode(NodeID nodeID)
{
    return mNodes[nodeID].mApp;
}
vector<Application::pointer>
Simulation::getNodes()
{
    vector<Application::pointer> result;
    for (auto const& p : mNodes)
        result.push_back(p.second.mApp);
    return result;
}
vector<NodeID>
Simulation::getNodeIDs()
{
    vector<NodeID> result;
    for (auto const& p : mNodes)
        result.push_back(p.first);
    return result;
}

void
Simulation::removeNode(NodeID const& id)
{
    auto it = mNodes.find(id);
    if (it != mNodes.end())
    {
        auto node = it->second;
        mPeerMap.erase(node.mApp->getConfig().PEER_PORT);
        mNodes.erase(it);
        node.mApp->gracefulStop();
        while (node.mClock->crank(false) > 0)
            ;
        if (mMode == OVER_LOOPBACK)
        {
            dropAllConnections(id);
        }
    }
}

Application::pointer
Simulation::getAppFromPeerMap(unsigned short peerPort)
{
    releaseAssert(mMode == OVER_LOOPBACK);
    auto it = mPeerMap.find(peerPort);
    if (it == mPeerMap.end())
    {
        return nullptr;
    }

    auto app = it->second.lock();
    if (app)
    {
        return app;
    }

    return nullptr;
}

void
Simulation::dropAllConnections(NodeID const& id)
{
    if (mMode == OVER_LOOPBACK)
    {
        assert(mPendingConnections.empty());
        mLoopbackConnections.erase(
            std::remove_if(mLoopbackConnections.begin(),
                           mLoopbackConnections.end(),
                           [&](std::shared_ptr<LoopbackPeerConnection> c) {
                               // use app's IDs here as connections may be
                               // incomplete
                               return c->getAcceptor()
                                              ->getConfig()
                                              .NODE_SEED.getPublicKey() == id ||
                                      c->getInitiator()
                                              ->getConfig()
                                              .NODE_SEED.getPublicKey() == id;
                           }),
            mLoopbackConnections.end());
    }
    else
    {
        throw std::runtime_error("can only drop connections over loopback");
    }
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
Simulation::dropConnection(NodeID initiator, NodeID acceptor)
{
    if (mMode == OVER_LOOPBACK)
        dropLoopbackConnection(initiator, acceptor);
    else
    {
        auto iApp = mNodes[initiator].mApp;
        if (iApp)
        {
            auto& cAcceptor = mNodes[acceptor].mApp->getConfig();

            auto peer = iApp->getOverlayManager().getConnectedPeer(
                PeerBareAddress{"127.0.0.1", cAcceptor.PEER_PORT});
            if (peer)
            {
                peer->drop("drop", Peer::DropDirection::WE_DROPPED_REMOTE);
            }
        }
    }
}

void
Simulation::addLoopbackConnection(NodeID initiator, NodeID acceptor)
{
    if (mNodes[initiator].mApp && mNodes[acceptor].mApp)
    {
        auto conn = std::make_shared<LoopbackPeerConnection>(
            *getNode(initiator), *getNode(acceptor));
        mLoopbackConnections.push_back(conn);
    }
}

std::shared_ptr<LoopbackPeerConnection>
Simulation::getLoopbackConnection(NodeID const& initiator,
                                  NodeID const& acceptor)
{
    auto it = std::find_if(
        std::begin(mLoopbackConnections), std::end(mLoopbackConnections),
        [&](std::shared_ptr<LoopbackPeerConnection> const& conn) {
            return conn->getInitiator()->getConfig().NODE_SEED.getPublicKey() ==
                       initiator &&
                   conn->getAcceptor()->getConfig().NODE_SEED.getPublicKey() ==
                       acceptor;
        });

    return it == std::end(mLoopbackConnections) ? nullptr : *it;
}

void
Simulation::dropLoopbackConnection(NodeID initiator, NodeID acceptor)
{
    auto it = std::find_if(
        std::begin(mLoopbackConnections), std::end(mLoopbackConnections),
        [&](std::shared_ptr<LoopbackPeerConnection> const& conn) {
            return conn->getInitiator()->getConfig().NODE_SEED.getPublicKey() ==
                       initiator &&
                   conn->getAcceptor()->getConfig().NODE_SEED.getPublicKey() ==
                       acceptor;
        });
    if (it != std::end(mLoopbackConnections))
    {
        mLoopbackConnections.erase(it);
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
    if (to->getConfig().PEER_PORT == 0)
    {
        throw runtime_error("PEER_PORT cannot be set to 0");
    }
    auto address = PeerBareAddress{"127.0.0.1", to->getConfig().PEER_PORT};
    from->getOverlayManager().connectTo(address);
}

void
Simulation::stopOverlayTick()
{
    auto cancel = [](Application::pointer app) {
        auto& ov = static_cast<OverlayManagerImpl&>(app->getOverlayManager());
        ov.mTimer.cancel();
    };
    cancel(mIdleApp);
    for (auto& n : mNodes)
    {
        cancel(n.second.mApp);
    }
}

void
Simulation::startAllNodes()
{
    for (auto const& it : mNodes)
    {
        auto app = it.second.mApp;
        if (app->getState() == Application::APP_CREATED_STATE)
        {
            app->start();
        }
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
        auto app = n.second.mApp;
        app->gracefulStop();
    }

    while (crankAllNodes() > 0)
        ;
}

size_t
Simulation::crankNode(NodeID const& id, VirtualClock::time_point timeout)
{
    auto p = mNodes[id];
    auto clock = p.mClock;
    auto app = p.mApp;
    if (app->getState() == Application::APP_CREATED_STATE)
    {
        throw std::runtime_error("Can't crank node that is not started");
    }

    size_t quantumClicks = 0;
    bool doneWithQuantum = false;
    VirtualTimer quantumTimer(*app);

    if (mVirtualClockMode)
    {
        // in virtual mode we give at most a timeslice
        // of quantum for execution
        auto tp = clock->now() + quantum;
        if (tp > timeout)
        {
            tp = timeout;
        }
        quantumTimer.expires_at(tp);
    }
    else
    {
        // real time means we only need to trigger whatever
        // we missed since the last time
        quantumTimer.expires_at(clock->now());
    }
    quantumTimer.async_wait([&](asio::error_code const& error) {
        doneWithQuantum = true;
        quantumClicks++;
    });

    size_t count = 0;
    while (!doneWithQuantum)
    {
        count += clock->crank(false);
    }

    // Update network survey phase
    OverlayManager& om = app->getOverlayManager();
    om.getSurveyManager().updateSurveyPhase(om.getInboundAuthenticatedPeers(),
                                            om.getOutboundAuthenticatedPeers(),
                                            app->getConfig());

    return count - quantumClicks;
}

std::size_t
Simulation::crankAllNodes(int nbTicks)
{

    std::size_t count = 0;

    VirtualTimer mainQuantumTimer(*mIdleApp);

    bool debugFmt = Logging::logDebug("Process");

    auto h = gsl::finally([&]() {
        if (debugFmt)
        {
            Logging::setFmt("<test>");
        }
    });
    int i = 0;
    do
    {
        // at this level, we want to advance the overall simulation
        // in some meaningful way (and not just by a quantum) nbTicks time

        // in virtual clock mode, this means advancing the clock until either
        // work was performed
        // or we've triggered the next scheduled event

        if (mClock.getIOContext().stopped())
        {
            return 0;
        }

        bool hasNext = (mClock.next() != mClock.next().max());
        int quantumClicks = 0;

        if (mVirtualClockMode)
        {
            // in virtual mode we need to crank the main clock manually
            mainQuantumTimer.expires_from_now(quantum);
            mainQuantumTimer.async_wait([&]() { quantumClicks++; },
                                        &VirtualTimer::onFailureNoop);
        }

        // now, run the clock on all nodes until their clock is caught up
        bool appBehind;
        // in virtual mode next interesting event is either a quantum click
        // or a scheduled event
        auto nextTime = mVirtualClockMode ? mClock.next() : mClock.now();
        do
        {
            // in real mode, this is equivalent to a simple loop
            appBehind = false;
            for (auto& p : mNodes)
            {
                auto clock = p.second.mClock;
                if (clock->getIOContext().stopped())
                {
                    continue;
                }

                hasNext = hasNext || (clock->next() != clock->next().max());

                if (mVirtualClockMode)
                {
                    auto appNow = clock->now();
                    if (appNow < nextTime)
                    {
                        appBehind = true;
                    }
                    else if (appNow >= nextTime)
                    {
                        // node caught up, don't give it any compute
                        continue;
                    }
                }
                if (debugFmt)
                {
                    Logging::setFmt(fmt::format(
                        "<test-{}>", p.second.mApp->getConfig().PEER_PORT));
                }
                crankNode(p.first, nextTime);
            }
        } while (appBehind);

        // let the main clock do its job
        count += mClock.crank(false);

        // don't count quantum slices
        count -= quantumClicks;

        // a tick is that either we've done work or
        // that we're in real clock mode
        // or that no event is scheduled
        if (count || !mVirtualClockMode || !hasNext)
        {
            i++;
        }
    } while (i < nbTicks);
    return count;
}

bool
Simulation::haveAllExternalized(uint32 num, uint32 maxSpread,
                                bool validatorsOnly)
{
    uint32_t min = UINT32_MAX, max = 0;
    for (auto it = mNodes.begin(); it != mNodes.end(); ++it)
    {
        auto app = it->second.mApp;
        if (validatorsOnly && !app->getConfig().NODE_IS_VALIDATOR)
        {
            continue;
        }
        auto n = app->getLedgerManager().getLastClosedLedgerNum();
        if (n < min)
            min = n;
        if (n > max)
            max = n;
    }
    if (max - min > maxSpread)
    {
        throw std::runtime_error(
            fmt::format("Too wide spread between nodes: {0}-{1} > {2}", max,
                        min, maxSpread));
    }
    return num <= min;
}

void
Simulation::crankForAtMost(VirtualClock::duration seconds, bool finalCrank)
{
    bool stop = false;
    auto stopIt = [&](asio::error_code const& error) {
        if (!error)
            stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop && crankAllNodes() > 0)
        ;

    if (stop)
        LOG_INFO(DEFAULT_LOG, "Simulation timed out");
    else
        LOG_INFO(DEFAULT_LOG, "Simulation complete");

    if (finalCrank)
    {
        stopAllNodes();
    }
}

void
Simulation::crankForAtLeast(VirtualClock::duration seconds, bool finalCrank)
{
    bool stop = false;
    auto stopIt = [&](asio::error_code const& error) {
        if (!error)
            stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_from_now(seconds);
    checkTimer.async_wait(stopIt);

    while (!stop)
    {
        if (crankAllNodes() == 0)
        {
            // this only happens when real time is configured
            std::this_thread::sleep_for(chrono::milliseconds(50));
        }
    }

    if (finalCrank)
    {
        stopAllNodes();
    }
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
    function<void()> checkDone = [&]() {
        if (predicate())
            done = true;
        else
        {
            checkTimer.expires_from_now(chrono::seconds(1));
            checkTimer.async_wait(checkDone, &VirtualTimer::onFailureNoop);
        }
    };

    timeoutTimer.async_wait(
        [&]() {
            checkDone();
            timedOut = true;
        },
        &VirtualTimer::onFailureNoop);

    // initial check, pre crank
    // mostly used for getting a snapshot of the
    // starting state and
    // being offset by a to avoid being too synchronized with apps
    checkTimer.expires_from_now(chrono::microseconds(100));
    checkTimer.async_wait(checkDone, &VirtualTimer::onFailureNoop);

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
Simulation::crankUntil(VirtualClock::time_point timePoint, bool finalCrank)
{
    bool stop = false;
    auto stopIt = [&](asio::error_code const& error) {
        if (!error)
            stop = true;
    };

    VirtualTimer checkTimer(*mIdleApp);

    checkTimer.expires_at(timePoint);
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
Simulation::crankUntil(VirtualClock::system_time_point timePoint,
                       bool finalCrank)
{
    crankUntil(VirtualClock::time_point(timePoint.time_since_epoch()),
               finalCrank);
}

Config
Simulation::newConfig()
{
    Config cfg;
    if (mConfigGen)
    {
        cfg = mConfigGen(mConfigCount++);
    }
    else
    {
        cfg = getTestConfig(mConfigCount++);
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    }

    return cfg;
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

bool
LoopbackOverlayManager::connectToImpl(PeerBareAddress const& address,
                                      bool forceoutbound)
{
    CLOG_TRACE(Overlay, "Connect to {}", address.toString());
    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection || (forceoutbound && currentConnection->getRole() ==
                                                    Peer::REMOTE_CALLED_US))
    {
        if (availableOutboundPendingSlots() <= 0)
        {
            CLOG_DEBUG(Overlay,
                       "Peer rejected - all outbound pending connections "
                       "taken: {}",
                       address.toString());
            return false;
        }
        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);
        auto& app = static_cast<ApplicationLoopbackOverlay&>(mApp);
        auto otherApp = app.getSim().getAppFromPeerMap(address.getPort());
        if (!otherApp)
        {
            return false;
        }
        auto res = LoopbackPeer::initiate(mApp, *otherApp);
        return res.first->isConnectedForTesting();
    }
    else
    {
        CLOG_ERROR(Overlay,
                   "trying to connect to a node we're already connected to {}",
                   address.toString());
        return false;
    }
}
}
