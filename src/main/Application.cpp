#include "Application.h"
#include "overlay/PeerMaster.h"
#include "lib/util/Logging.h"
#include "util/make_unique.h"

namespace stellar
{
    Application::Application(Config const& cfg) :
        mState(BOOTING_STATE),
        mConfig(cfg),
        mPeerMaster(*this),
        mTxHerder(*this),
        mFBAMaster(*this),
        mMainThread(cfg.SINGLE_STEP_MODE ?
                    nullptr :
                    std::move(make_unique<std::thread>([this](){ this->runMainThread(); }))),
        mStopSignals(mMainIOService, SIGINT)
    {
        mStopSignals.async_wait([this](asio::error_code const& ec, int sig) {
                LOG(INFO) << "got signal " << sig << ", shutting down";
                this->gracefulStop();
            });
        unsigned t = std::thread::hardware_concurrency();
        if (!mConfig.SINGLE_STEP_MODE)
        {
            LOG(INFO) << "Worker threads: " << t;
            while (t--) {
                mWorkerThreads.emplace_back([this, t]() { this->runWorkerThread(t); });
            }
        }
    }

    void Application::runWorkerThread(unsigned i)
    {
        if (mConfig.SINGLE_STEP_MODE)
            return;

        LOG(INFO) << "Worker thread " << i << " starting";
        mWorkerIOService.run();
        LOG(INFO) << "Worker thread " << i << " complete";
    }

    void Application::runMainThread()
    {
        if (mConfig.SINGLE_STEP_MODE)
            return;

        LOG(INFO) << "Main thread starting";
        mMainIOService.run();
        LOG(INFO) << "Main thread complete";
    }

    void Application::gracefulStop()
    {
        LOG(INFO) << "Graceful stop requested";

        LOG(INFO) << "Stopping worker IO service";
        mWorkerIOService.stop();

        LOG(INFO) << "Stopping main IO service";
        mMainIOService.stop();
    }

    void Application::joinAllThreads()
    {
        if (mConfig.SINGLE_STEP_MODE)
            return;

        unsigned i = 0;
        for (auto &w : mWorkerThreads)
        {
            LOG(INFO) << "Joining worker thread " << i++;
            w.join();
        }

        LOG(INFO) << "Joining main thread ";
        mMainThread->join();

        LOG(INFO) << "All threads complete";
    }
}
