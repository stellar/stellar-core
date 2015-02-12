// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#define STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "util/Timer.h"

#include "Application.h"

#include "ledger/LedgerMaster.h"
#include "main/Config.h"
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

struct Application::Impl
{
    Application::State mState;
    VirtualClock& mVirtualClock;
    Config mConfig;

    // NB: The io_services should come first, then the 'master'
    // sub-objects, then the threads. Do not reorder these fields.
    //
    // The fields must be constructed in this order, because the
    // 'master' sub-objects register work-to-do (listening on sockets)
    // with the io_services during construction, and the threads are
    // activated immediately thereafter to serve requests; if the
    // threads started first, they would try to do work, find no work,
    // and exit.
    //
    // The fields must be destructed in the reverse order because the
    // 'master' sub-objects contain various IO objects that refer
    // directly to the io_services.

    asio::io_service mMainIOService;
    asio::io_service mWorkerIOService;
    std::unique_ptr<asio::io_service::work> mWork;
    std::unique_ptr<RealTimer> mRealTimer;

    std::unique_ptr<medida::MetricsRegistry> mMetrics;
    std::unique_ptr<TmpDirMaster> mTmpDirMaster;
    std::unique_ptr<PeerMaster> mPeerMaster;
    std::unique_ptr<LedgerMaster> mLedgerMaster;
    std::unique_ptr<Herder> mHerder;
    std::unique_ptr<CLFMaster> mCLFMaster;
    std::unique_ptr<HistoryMaster> mHistoryMaster;
    std::unique_ptr<ProcessMaster> mProcessMaster;
    std::unique_ptr<CommandHandler> mCommandHandler;
    std::unique_ptr<Database> mDatabase;

    std::vector<std::thread> mWorkerThreads;

    asio::signal_set mStopSignals;
    size_t mRealTimerCancelCallbacks;

    void runWorkerThread(unsigned i);
    void scheduleRealTimerFromNextVirtualEvent();
    size_t advanceVirtualTimeToRealTime();
    void realTimerTick();

    void enableRealTimer();
    void disableRealTimer();
    size_t crank(bool block=true);

    void start();
    void gracefulStop();
    void joinAllThreads();
    void applyCfgCommands();

    Impl(VirtualClock& clock, Config const& cfg);
    ~Impl();
};

Application::Impl::Impl(VirtualClock& clock, Config const& cfg)
    : mState(Application::State::BOOTING_STATE)
    , mVirtualClock(clock)
    , mConfig(cfg)
    , mMainIOService()
    , mWorkerIOService(std::thread::hardware_concurrency())
    , mWork(make_unique<asio::io_service::work>(mWorkerIOService))
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
    while(t--)
    {
        mWorkerThreads.emplace_back([this, t]()
        {
            this->runWorkerThread(t);
        });
    }
    LOG(INFO) << "Application constructed";

}

Application::Impl::~Impl()
{
    LOG(INFO) << "Application destructing";
    gracefulStop();
    joinAllThreads();
    LOG(INFO) << "Application destroyed";
}

Application::Application(VirtualClock& clock, Config const& cfg)
    : mImpl(make_unique<Impl>(clock, cfg))
{
    // These must be constructed _after_ mImpl points to a
    // full Impl object, because they frequently call back
    // into App.getFoo() to get information / start up.
    mImpl->mTmpDirMaster = make_unique<TmpDirMaster>(*this);
    mImpl->mPeerMaster = make_unique<PeerMaster>(*this);
    mImpl->mLedgerMaster = make_unique<LedgerMaster>(*this);
    mImpl->mHerder = make_unique<Herder>(*this);
    mImpl->mCLFMaster = make_unique<CLFMaster>(*this);
    mImpl->mHistoryMaster = make_unique<HistoryMaster>(*this);
    mImpl->mProcessMaster = make_unique<ProcessMaster>(*this);
    mImpl->mCommandHandler = make_unique<CommandHandler>(*this);
    mImpl->mDatabase = make_unique<Database>(*this);
}

void
Application::enableRealTimer()
{
    mImpl->enableRealTimer();
}

void
Application::Impl::enableRealTimer()
{
    if (!mRealTimer)
    {
        mRealTimer = make_unique<RealTimer>(mMainIOService);
        realTimerTick();
    }
}

void
Application::disableRealTimer()
{
    mImpl->disableRealTimer();
}

void
Application::Impl::disableRealTimer()
{
    if (mRealTimer)
    {
        mRealTimer.reset();
    }
}

void
Application::Impl::realTimerTick()
{
    // LOG(DEBUG) << "realTimerTick";
    advanceVirtualTimeToRealTime();
    scheduleRealTimerFromNextVirtualEvent();
}

void
Application::Impl::scheduleRealTimerFromNextVirtualEvent()
{
    if (mRealTimer)
    {
        auto nxt = mVirtualClock.next();
        // Cancels any pending wait, and sets a new one.
        // LOG(DEBUG) << "Application::scheduleRealTimerFromNextVirtualEvent "
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
Application::Impl::advanceVirtualTimeToRealTime()
{
    auto n = std::chrono::steady_clock::now();
    // LOG(DEBUG) << "Application::advanceVirtualTimeToRealTime";
    return mVirtualClock.advanceTo(n);
}

size_t
Application::crank(bool block)
{
    return mImpl->crank(block);
}

size_t
Application::Impl::crank(bool block)
{
    // LOG(DEBUG) << "Application::crank(block=" << block << ")";
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

    // LOG(DEBUG) << "Application::cranked: " << nWorkDone;
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
Application::start()
{
    mImpl->start();
}

void
Application::Impl::start()
{
    if(mConfig.START_NEW_NETWORK)
    {
        mLedgerMaster->startNewLedger();
        mHerder->bootstrap();
    }
    else
    {
        mLedgerMaster->loadLastKnownLedger();
    }
}

Application::~Application()
{
    // We have to do this manually because some of the dtors
    // of the sub-objects call back through here into the
    // being-deleted application object, and the default
    // dtor for unique_ptr clears itself first.
    delete mImpl.get();
    mImpl.release();
}

void
Application::Impl::runWorkerThread(unsigned i)
{
    mWorkerIOService.run();
}

void
Application::gracefulStop()
{
    mImpl->gracefulStop();
}

void
Application::Impl::gracefulStop()
{
    if (!mMainIOService.stopped())
    {
        LOG(INFO) << "Stopping main IO service";
        mMainIOService.stop();
    }
}

void
Application::joinAllThreads()
{
    mImpl->joinAllThreads();
}

void
Application::Impl::joinAllThreads()
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

void Application::Impl::applyCfgCommands()
{
    for(auto cmd : mConfig.COMMANDS)
    {
        mCommandHandler->manualCmd(cmd);
    }
}

Config const&
Application::getConfig()
{
    return mImpl->mConfig;
}

Application::State
Application::getState()
{
    return mImpl->mState;
}

void
Application::setState(State s)
{
    mImpl->mState = s;
}

VirtualClock&
Application::getClock()
{
    return mImpl->mVirtualClock;
}

medida::MetricsRegistry&
Application::getMetrics()
{
    return *mImpl->mMetrics;
}

TmpDirMaster&
Application::getTmpDirMaster()
{
    return *mImpl->mTmpDirMaster;
}

LedgerGateway&
Application::getLedgerGateway()
{
    return *mImpl->mLedgerMaster;
}

LedgerMaster&
Application::getLedgerMaster()
{
    return *mImpl->mLedgerMaster;
}

CLFMaster&
Application::getCLFMaster()
{
    return *mImpl->mCLFMaster;
}

HistoryMaster&
Application::getHistoryMaster()
{
    return *mImpl->mHistoryMaster;
}

ProcessGateway&
Application::getProcessGateway()
{
    return *mImpl->mProcessMaster;
}

HerderGateway&
Application::getHerderGateway()
{
    return *mImpl->mHerder;
}

OverlayGateway&
Application::getOverlayGateway()
{
    return *mImpl->mPeerMaster;
}

PeerMaster&
Application::getPeerMaster()
{
    return *mImpl->mPeerMaster;
}

Database&
Application::getDatabase()
{
    return *mImpl->mDatabase;
}

asio::io_service&
Application::getMainIOService()
{
    return mImpl->mMainIOService;
}

asio::io_service&
Application::getWorkerIOService()
{
    return mImpl->mWorkerIOService;
}

void Application::applyCfgCommands()
{
    mImpl->applyCfgCommands();
}

}
