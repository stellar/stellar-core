#include "Application.h"
#include "overlay/PeerMaster.h"
#include "lib/util/Logging.h"
#include "util/make_unique.h"

namespace stellar
{
Application::Application(Config const& cfg)
    : mState(BOOTING_STATE)
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
    while (t--)
    {
        mWorkerThreads.emplace_back([this, t]()
                                    {
                                        this->runWorkerThread(t);
                                    });
    }
    LOG(INFO) << "Application constructed";
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
