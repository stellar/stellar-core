// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#define STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "ApplicationImpl.h"

#include "ledger/LedgerMaster.h"
#include "herder/Herder.h"
#include "overlay/OverlayGateway.h"
#include "overlay/PeerMaster.h"
#include "clf/CLFMaster.h"
#include "history/HistoryMaster.h"
#include "database/Database.h"
#include "process/ProcessMaster.h"
#include "main/CommandHandler.h"
#include "medida/metrics_registry.h"

#include "util/TmpDir.h"
#include "util/Logging.h"
#include "util/make_unique.h"

namespace stellar
{


ApplicationImpl::ApplicationImpl(VirtualClock& clock, Config const& cfg)
    : mState(Application::State::BOOTING_STATE)
    , mVirtualClock(clock)
    , mConfig(cfg)
    , mMainIOService()
    , mWorkerIOService(std::thread::hardware_concurrency())
    , mWork(make_unique<asio::io_service::work>(mWorkerIOService))
    , mWorkerThreads()
    , mStopSignals(mMainIOService, SIGINT)
    , mRealTimerCancelCallbacks(0)
{
#ifdef SIGQUIT
    mStopSignals.add(SIGQUIT);
#endif
#ifdef SIGTERM
    mStopSignals.add(SIGTERM);
#endif

    unsigned t = std::thread::hardware_concurrency();
    LOG(INFO) << "Application constructing "
              << "(worker threads: " << t << ")";
    mStopSignals.async_wait([this](asio::error_code const& ec, int sig)
    {
        LOG(INFO) << "got signal " << sig
            << ", shutting down";
        this->gracefulStop();
    });

    // These must be constructed _after_ because they frequently call back
    // into App.getFoo() to get information / start up.
    mMetrics = make_unique<medida::MetricsRegistry>();
    mTmpDirMaster = make_unique<TmpDirMaster>(cfg.TMP_DIR_PATH);
    mPeerMaster = make_unique<PeerMaster>(*this);
    mLedgerMaster = make_unique<LedgerMaster>(*this);
    mHerder = make_unique<Herder>(*this);
    mCLFMaster = make_unique<CLFMaster>(*this);
    mHistoryMaster = make_unique<HistoryMaster>(*this);
    mProcessMaster = make_unique<ProcessMaster>(*this);
    mCommandHandler = make_unique<CommandHandler>(*this);
    mDatabase = make_unique<Database>(*this);

    while(t--)
    {
        mWorkerThreads.emplace_back([this, t]()
        {
            this->runWorkerThread(t);
        });
    }

    LOG(INFO) << "Application constructed";

}

ApplicationImpl::~ApplicationImpl()
{
    LOG(INFO) << "Application destructing";
    gracefulStop();
    joinAllThreads();
    LOG(INFO) << "Application destroyed";
}

void
ApplicationImpl::enableRealTimer()
{
    if (!mRealTimer)
    {
        mRealTimer = make_unique<RealTimer>(mMainIOService);
        realTimerTick();
    }
}

void
ApplicationImpl::disableRealTimer()
{
    if (mRealTimer)
    {
        mRealTimer.reset();
    }
}

uint64_t ApplicationImpl::timeNow()
{
    return getClock().now().time_since_epoch().count() * std::chrono::system_clock::period::num / std::chrono::system_clock::period::den;
}

void
ApplicationImpl::realTimerTick()
{
    // LOG(DEBUG) << "realTimerTick";
    advanceVirtualTimeToRealTime();
    scheduleRealTimerFromNextVirtualEvent();
}

void
ApplicationImpl::scheduleRealTimerFromNextVirtualEvent()
{
    if (mRealTimer)
    {
        auto nxt = mVirtualClock.next();
        // Cancels any pending wait, and sets a new one.
        // LOG(DEBUG) << "ApplicationImpl::scheduleRealTimerFromNextVirtualEvent "
        //           << "sleeping realTimer until "
        //           << nxt.time_since_epoch().count();
        mRealTimer->expires_at(nxt);
        mRealTimer->async_wait([this](asio::error_code ec)
                                      {
                                          if (ec == asio::error::operation_aborted)
                                          {
                                              this->mRealTimerCancelCallbacks++;
                                          }
                                          else
                                          {
                                              this->realTimerTick();
                                          }
                                      });
    }
}

size_t
ApplicationImpl::advanceVirtualTimeToRealTime()
{
    auto n = std::chrono::steady_clock::now();
    // LOG(DEBUG) << "ApplicationImpl::advanceVirtualTimeToRealTime";
    return mVirtualClock.advanceTo(n);
}

size_t
ApplicationImpl::crank(bool block)
{
    // LOG(DEBUG) << "ApplicationImpl::crank(block=" << block << ")";
    mRealTimerCancelCallbacks = 0;
    size_t nWorkDone = 0;
    if (block)
    {
        nWorkDone = mMainIOService.run_one();
    }
    else
    {
        nWorkDone = mMainIOService.poll_one();
    }

    // NB: We do not consider "canceling the realtimer" as a form of "real
    // work", partly because it's just plain uninteresting work, and partly
    // because we actually queue up such a cancellation below _as part of_
    // rescheduling the realtimer, when there _was_ "real work" done; if we
    // consider the cancellation as work, we get a feedback loop chasing
    // cancellation events.
    nWorkDone -= mRealTimerCancelCallbacks;
    mRealTimerCancelCallbacks = 0;

    // LOG(DEBUG) << "ApplicationImpl::cranked: " << nWorkDone;
    if (mRealTimer && nWorkDone != 0)
    {
        // If we have a real timer, we let its sleep events _primarily_ drive
        // the sleeping-and-waking of the main loop; but any time we did real
        // work in an event-loop crank we treat it as a timer tick to give
        // virtual time the opportunity to catch up (in case real time overshot
        // it while we were doing slow work), and to ensure that the real timer
        // is (re)scheduled for the next virtual-time wakeup (in case some new
        // virtual timers were scheduled during the most recent work).
        realTimerTick();
    }
    else if (!mRealTimer && nWorkDone == 0)
    {
        // If we have no real-timer driving virtual time, then we advance
        // virtual time whenever we run out of immediate (I/O event loop) work
        // to do; this means that in simulated-time all I/O and computation
        // happens "instantly" between two time steps and then virtual time skips
        // forward to the next scheduled event as soon as we're idle.
        return mVirtualClock.advanceToNext();
    }
    return nWorkDone;
}

void
ApplicationImpl::start()
{
    if(mConfig.START_NEW_NETWORK)
    {
        mLedgerMaster->startNewLedger();
        mHerder->bootstrap();
    }else if(mConfig.START_LOCAL_NETWORK)
    {
        mLedgerMaster->loadLastKnownLedger();
        mHerder->bootstrap();
    }else
    {
        mLedgerMaster->loadLastKnownLedger();
    }
}

void
ApplicationImpl::runWorkerThread(unsigned i)
{
    mWorkerIOService.run();
}

void
ApplicationImpl::gracefulStop()
{
    // Drain all events queued to fire on this app.
    while (mVirtualClock.cancelAllEventsFrom(*this));

    if (!mMainIOService.stopped())
    {
        LOG(INFO) << "Stopping main IO service";
        mMainIOService.stop();
    }
}

void
ApplicationImpl::joinAllThreads()
{
    // We never strictly stop the worker IO service, just release the work-lock
    // that keeps the worker threads alive. This gives them the chance to finish
    // any work that the main thread queued.
    if (mWork)
    {
        mWork.reset();
    }
    LOG(INFO) << "Joining " << mWorkerThreads.size() << " worker threads";
    for (auto& w : mWorkerThreads)
    {
        w.join();
    }
    LOG(INFO) << "Joined all " << mWorkerThreads.size() << " threads";
}

void ApplicationImpl::applyCfgCommands()
{
    for(auto cmd : mConfig.COMMANDS)
    {
        mCommandHandler->manualCmd(cmd);
    }
}

Config const&
ApplicationImpl::getConfig()
{
    return mConfig;
}

Application::State
ApplicationImpl::getState()
{
    return mState;
}

void
ApplicationImpl::setState(State s)
{
    mState = s;
}

VirtualClock&
ApplicationImpl::getClock()
{
    return mVirtualClock;
}

medida::MetricsRegistry&
ApplicationImpl::getMetrics()
{
    return *mMetrics;
}

TmpDirMaster&
ApplicationImpl::getTmpDirMaster()
{
    return *mTmpDirMaster;
}

LedgerGateway&
ApplicationImpl::getLedgerGateway()
{
    return *mLedgerMaster;
}

LedgerMaster&
ApplicationImpl::getLedgerMaster()
{
    return *mLedgerMaster;
}

CLFMaster&
ApplicationImpl::getCLFMaster()
{
    return *mCLFMaster;
}

HistoryMaster&
ApplicationImpl::getHistoryMaster()
{
    return *mHistoryMaster;
}

ProcessGateway&
ApplicationImpl::getProcessGateway()
{
    return *mProcessMaster;
}

HerderGateway&
ApplicationImpl::getHerderGateway()
{
    return *mHerder;
}

OverlayGateway&
ApplicationImpl::getOverlayGateway()
{
    return *mPeerMaster;
}

PeerMaster&
ApplicationImpl::getPeerMaster()
{
    return *mPeerMaster;
}

Database&
ApplicationImpl::getDatabase()
{
    return *mDatabase;
}

asio::io_service&
ApplicationImpl::getMainIOService()
{
    return mMainIOService;
}

asio::io_service&
ApplicationImpl::getWorkerIOService()
{
    return mWorkerIOService;
}

}
