#include "Application.h"
#include "overlay/PeerMaster.h"
#include "lib/util/Logging.h"
#include "util/make_unique.h"

namespace stellar
{
Application::Application(Config const &cfg)
    : mState(BOOTING_STATE),
      mConfig(cfg),
      mWorkerIOService(std::thread::hardware_concurrency()),
      mWork(make_unique<asio::io_service::work>(mWorkerIOService)),
      mPeerMaster(*this),
      mTxHerder(*this),
      mFBAMaster(*this),
      mLedgerMaster(*this),
      mStopSignals(mMainIOService, SIGINT)
{
    LOG(INFO) << "Application constructing";
    mStopSignals.async_wait([this](asio::error_code const &ec, int sig)
                            {
                                LOG(INFO) << "got signal " << sig
                                          << ", shutting down";
                                this->gracefulStop();
                            });
    unsigned t = std::thread::hardware_concurrency();
    LOG(INFO) << "Worker threads: " << t;
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
    LOG(INFO) << "Worker thread " << i << " starting";
    mWorkerIOService.run();
    LOG(INFO) << "Worker thread " << i << " complete";
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
        LOG(INFO) << "Releasing worker threads";
        mWork.reset();
    }
    unsigned i = 0;
    for (auto &w : mWorkerThreads)
    {
        LOG(INFO) << "Joining worker thread " << i++;
        w.join();
    }

    LOG(INFO) << "All worker threads complete";
}
}
