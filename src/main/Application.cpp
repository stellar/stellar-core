// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#define STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "util/Timer.h"

#include "Application.h"
#include "overlay/PeerMaster.h"
#include "util/Logging.h"
#include "util/make_unique.h"

namespace stellar
{

Application::Application(VirtualClock& clock, Config const& cfg)
    : mState(BOOTING_STATE)
    , mVirtualClock(clock)
    , mConfig(cfg)
    , mWorkerIOService(std::thread::hardware_concurrency())
    , mWork(make_unique<asio::io_service::work>(mWorkerIOService))
    , mPeerMaster(*this)
    , mLedgerMaster(*this)
    , mTxHerder(*this)
    , mFBAMaster(*this)
    , mHistoryMaster(*this)
    , mProcessMaster(*this)
    , mCommandHandler(*this)
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

void
Application::enableRealTimer()
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
    if (mRealTimer)
    {
        mRealTimer.reset();
    }
}

void
Application::realTimerTick()
{
    // LOG(DEBUG) << "realTimerTick";
    advanceVirtualTimeToRealTime();
    scheduleRealTimerFromNextVirtualEvent();
}

void
Application::scheduleRealTimerFromNextVirtualEvent()
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
Application::advanceVirtualTimeToRealTime()
{
    auto n = std::chrono::steady_clock::now();
    // LOG(DEBUG) << "Application::advanceVirtualTimeToRealTime";
    return mVirtualClock.advanceTo(n);
}

size_t
Application::crank(bool block)
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

    // NB: We do not consider "cancelling the realtimer" as a form of "real
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
    if(mConfig.START_NEW_NETWORK)
    {
        LOG(INFO) << "Starting a new network";
        mLedgerMaster.startNewLedger();
    }
}

Application::~Application()
{
    LOG(INFO) << "Application destructing";
    gracefulStop();
    joinAllThreads();
    LOG(INFO) << "Application destroyed";
}

void
Application::runWorkerThread(unsigned i)
{
    mWorkerIOService.run();
}

void
Application::gracefulStop()
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
}
