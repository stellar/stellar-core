// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Simulation.h"

#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "overlay/RustOverlayManager.h"
#include "scp/LocalNode.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/finally.h"
#include "util/types.h"

#include <fmt/format.h>

#include "main/ApplicationUtils.h"
#include "medida/medida.h"
#include "medida/reporting/console_reporter.h"
#include "util/Logging.h"

#include <thread>

namespace stellar
{

using namespace std;

Simulation::Simulation(Hash const& networkID, ConfigGen confGen,
                       QuorumSetAdjuster qSetAdjust)
    : mClock(VirtualClock::REAL_TIME)
    , mConfigCount(0)
    , mConfigGen(confGen)
    , mQuorumSetAdjuster(qSetAdjust)
{
    auto cfg = newConfig();
    mIdleApp = Application::create(mClock, cfg);
}

Simulation::~Simulation()
{
    // for (auto& node : mNodes)
    // {
    //     node.second.mApp->gracefulStop();
    //     if (node.second.mApp->getState() == Application::APP_CREATED_STATE)
    //     {
    //         continue;
    //     }
    //     crankUntil([node] { return node.second.mApp->getClock().isStopped();
    //     },
    //                std::chrono::seconds(20), false);
    // }

    // destroy all nodes first
    mNodes.clear();

    mIdleApp->gracefulStop();
    // if (mIdleApp->getState() != Application::APP_CREATED_STATE)
    // {
    //     crankUntil([this] { return mIdleApp->getClock().isStopped(); },
    //                std::chrono::seconds(20), false);
    // }
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
Simulation::addNode(SecretKey nodeKey, QuorumSetSpec qSet, Config const* cfg2,
                    bool newDB)
{
    auto cfg = cfg2 ? std::make_shared<Config>(*cfg2)
                    : std::make_shared<Config>(newConfig());
    cfg->adjust();
    cfg->NODE_SEED = nodeKey;
    cfg->MANUAL_CLOSE = false;
    cfg->RUN_STANDALONE = false;

    // Binary path for Rust overlay
    cfg->OVERLAY_BINARY_PATH = "../target/release/stellar-overlay";

    if (SCPQuorumSet const* manualQSet = std::get_if<SCPQuorumSet>(&qSet))
    {
        if (mQuorumSetAdjuster)
        {
            cfg->QUORUM_SET = mQuorumSetAdjuster(*manualQSet);
        }
        else
        {
            cfg->QUORUM_SET = *manualQSet;
        }
    }
    else
    {
        // Auto quorum set configuration is incompatible with
        // `QuorumSetAdjuster`
        releaseAssert(!mQuorumSetAdjuster);

        auto const& validators = std::get<std::vector<ValidatorEntry>>(qSet);
        cfg->SKIP_HIGH_CRITICAL_VALIDATOR_CHECKS_FOR_TESTING = true;
        cfg->generateQuorumSetForTesting(validators);
    }

    auto clock = make_shared<VirtualClock>(VirtualClock::REAL_TIME);

    Application::pointer app =
        createTestApplication(*clock, *cfg, newDB, false);

    mNodes.emplace(nodeKey.getPublicKey(), Node{clock, app});

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
        mNodes.erase(it);
        node.mApp->gracefulStop();
        while (node.mClock->crank(false) > 0)
            ;
    }
}

std::chrono::milliseconds
Simulation::getExpectedLedgerCloseTime() const
{
    if (mNodes.empty())
    {
        return Herder::TARGET_LEDGER_CLOSE_TIME_BEFORE_PROTOCOL_VERSION_23_MS;
    }

    // Pick arbitrary app
    auto const& node = mNodes.begin()->second.mApp;
    return node->getLedgerManager().getExpectedLedgerCloseTime();
}

void
Simulation::startAllNodes()
{
    for (auto const& it : mNodes)
    {
        auto app = it.second.mApp;
        if (app->getState() == Application::APP_CREATED_STATE)
        {
            CLOG_INFO(Herder, "Starting node {}", app->getConfig().PEER_PORT);
            app->start();
        }
    }
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

    // real time means we only need to trigger whatever
    // we missed since the last time
    quantumTimer.expires_at(clock->now());
    quantumTimer.async_wait([&](asio::error_code const& error) {
        doneWithQuantum = true;
        quantumClicks++;
    });

    size_t count = 0;
    while (!doneWithQuantum)
    {
        count += clock->crank(false);
    }

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
        if (mClock.getIOContext().stopped())
        {
            return 0;
        }

        bool hasNext = (mClock.next() != mClock.next().max());
        int quantumClicks = 0;

        // now, run the clock on all nodes until their clock is caught up
        bool appBehind;
        // in virtual mode next interesting event is either a quantum click
        // or a scheduled event
        auto nextTime = mClock.now();
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

                if (debugFmt)
                {
                    Logging::setFmt(fmt::format(
                        "<test-{}>", p.second.mApp->getConfig().PEER_PORT));
                }
                crankNode(p.first, nextTime);
            }
        } while (appBehind);

        count += mClock.crank(false);

        // don't count quantum slices
        count -= quantumClicks;
        i++;
    } while (i < nbTicks);
    return count;
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

void
Simulation::crankForAtMost(VirtualClock::duration seconds, bool finalCrank)
{
    crankUntil(mClock.now() + seconds, finalCrank);
}

void
Simulation::crankForAtLeast(VirtualClock::duration seconds, bool finalCrank)
{
    crankUntil(mClock.now() + seconds, finalCrank);
}

bool
Simulation::haveAllExternalized(uint32 num, uint32 maxSpread,
                                bool validatorsOnly)
{
    uint32_t min = UINT32_MAX, max = 0;
    for (auto it = mNodes.begin(); it != mNodes.end(); ++it)
    {
        auto app = it->second.mApp;
        auto validating = app->getConfig().NODE_IS_VALIDATOR;
        if (!validatorsOnly || validating)
        {
            auto n = app->getLedgerManager().getLastClosedLedgerNum();
            if (n < min)
                min = n;
            if (n > max)
                max = n;
        }
    }
    if (min > num + maxSpread)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("%.0 overshoot in simulation: min {:d}, expected {:d}"),
            min, num));
    }
    return (min >= num) && ((max - min) <= maxSpread);
}

Config
Simulation::newConfig()
{
    if (mConfigGen)
    {
        return mConfigGen(mConfigCount++);
    }
    else
    {
        Config res = getTestConfig(mConfigCount++);
        res.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        return res;
    }
}

class ConsoleReporterWithSum : public medida::MetricProcessor
{
    std::ostream& out_;

  public:
    ConsoleReporterWithSum(medida::MetricsRegistry& registry,
                           std::ostream& out = std::cerr)
        : out_(out)
    {
    }

    virtual ~ConsoleReporterWithSum()
    {
    }

    virtual void
    Process(medida::Counter& counter)
    {
        out_ << "           count = " << counter.count() << endl;
    }

    virtual void
    Process(medida::Meter& meter)
    {
        auto unit = " events/" + meter.event_type();
        auto mean_rate = meter.mean_rate();
        out_ << "           count = " << meter.count() << endl
             << "       mean rate = " << mean_rate << unit << endl
             << "   1-minute rate = " << meter.one_minute_rate() << unit << endl
             << "   5-minute rate = " << meter.five_minute_rate() << unit
             << endl
             << "  15-minute rate = " << meter.fifteen_minute_rate() << unit
             << endl;
    }

    virtual void
    Process(medida::Histogram& histogram)
    {
        auto snapshot = histogram.GetSnapshot();
        out_ << "             min = " << histogram.min() << endl
             << "             max = " << histogram.max() << endl
             << "            mean = " << histogram.mean() << endl
             << "          stddev = " << histogram.std_dev() << endl
             << "          median = " << snapshot.getMedian() << endl
             << "             75% = " << snapshot.get75thPercentile() << endl
             << "             95% = " << snapshot.get95thPercentile() << endl
             << "             98% = " << snapshot.get98thPercentile() << endl
             << "             99% = " << snapshot.get99thPercentile() << endl
             << "           99.9% = " << snapshot.get999thPercentile() << endl;
    }

    virtual void
    Process(medida::Timer& timer)
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
