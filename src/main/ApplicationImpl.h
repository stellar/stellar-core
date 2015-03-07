#pragma once

#define STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "util/Timer.h"
#include "Application.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include <thread>

namespace stellar
{
class TmpDirMaster;
class PeerMaster;
class LedgerMaster;
class Herder;
class CLFMaster;
class HistoryMaster;
class ProcessMaster;
class CommandHandler;
class Database;
    
class ApplicationImpl : public Application
{
public:
    ApplicationImpl(VirtualClock& clock, Config const& cfg);
    virtual ~ApplicationImpl() override;

    virtual size_t crank(bool block = true) override;
    virtual uint64_t timeNow() override;

    virtual Config const& getConfig() override;

    virtual State getState() override;
    virtual void setState(State) override;
    virtual VirtualClock& getClock() override;
    virtual medida::MetricsRegistry& getMetrics() override;
    virtual TmpDirMaster& getTmpDirMaster() override;
    virtual LedgerGateway& getLedgerGateway() override;
    virtual LedgerMaster& getLedgerMaster() override;
    virtual CLFMaster& getCLFMaster() override;
    virtual HistoryMaster& getHistoryMaster() override;
    virtual ProcessGateway& getProcessGateway() override;
    virtual HerderGateway& getHerderGateway() override;
    virtual OverlayGateway& getOverlayGateway() override;
    virtual PeerMaster& getPeerMaster() override;
    virtual Database& getDatabase() override;
    virtual PersistentState& getPersistentState() override;

    virtual asio::io_service& getMainIOService() override;
    virtual asio::io_service& getWorkerIOService() override;

    virtual void start() override;

    // Stops the io_services, which should cause the threads to exit
    // once they finish running any work-in-progress. If you want a
    // more abrupt exit than this, call exit() and hope for the best.
    virtual void gracefulStop() override;

    // Wait-on and join all the threads this application started; should
    // only return when there is no more work to do or someone has
    // force-stopped the io_services. Application can be safely destroyed
    // after this returns.
    virtual void joinAllThreads() override;


    virtual void applyCfgCommands() override;

private:
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

    asio::io_service mWorkerIOService;
    std::unique_ptr<asio::io_service::work> mWork;

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
    std::unique_ptr<PersistentState> mPersistentState;

    std::vector<std::thread> mWorkerThreads;

    asio::signal_set mStopSignals;

    void runWorkerThread(unsigned i);
};

}
