#ifndef __APPLICATION__
#define __APPLICATION__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include "fba/FBAMaster.h"
#include "ledger/LedgerMaster.h"
#include "main/Config.h"
#include "txherder/TxHerder.h"
#include "overlay/OverlayGateway.h"
#include "overlay/PeerMaster.h"
#include "clf/BucketList.h"
#include "history/HistoryMaster.h"
#include "database/Database.h"
#include "process/ProcessMaster.h"
#include "main/CommandHandler.h"

/*
 * State of a single instance of the application.
 *
 * Multiple instances may exist in the same process, eg. for the sake of
 * testing by simulating a network of applications.
 *
 * Owns two asio::io_services, one "main" (driven by the main thread) and one
 * "worker" (driven by a pool of #NCORE worker threads). The main io_service
 * has the run of the application and responds to the majority of (small,
 * sequential, consensus-related) network requests. The worker
 * threads/io_service are for long-running, self-contained helper jobs such as
 * bulk transfers and hashing. They should not touch anything outside their own
 * job-state (i.e. in a closure) and should post results back to the main
 * io_service when complete.
 *
 */

namespace stellar
{

class VirtualClock;

class Application : public enable_shared_from_this<Application>
{
  public:
    typedef std::shared_ptr<Application> pointer;

    // State invariants / definitions:
    //
    //  - Define "trusted" as "something signed by a sufficient set
    //    of parties based on our _current_ config-file quorum-set".
    //    This definition may change from run to run. This is intentional.
    //    Trust is not permanent, may need to be reinforced by some
    //    other party if we stop trusting someone we trusted in the past.
    //
    //  - Catching-up means: the newest trusted ledger we have on hand has a
    //    sequence number less than the highest "previous-ledger" sequence
    //    number we hear in ballots from any of our quorum-sets. In other
    //    words, we don't have the prestate necessary to run consensus
    //    transactions against yet, even if we wanted to.
    //
    //  - We only ever execute a transaction set when it's part of a
    //    trusted ledger. Currently trusted, not historical trusted.
    //    This includes the current consensus round: we don't run the
    //    transactions at all until we're certain everyone agrees on them.
    //
    //  - We only ever place our signature on a ledger when we have executed
    //    the transactions ourselves and verified the outcome. Even if we
    //    trust someone else's signatures for the sake of constructing a
    //    ledger (say, from snapshots), we don't _add our own signature_
    //    without execution as well.

    enum State
    {
        BOOTING_STATE,     // loading last known ledger from disk
        CONNECTING_STATE,  // trying to connect to other peers
        CONNECTED_STATE,   // connected to other peers and receiving validations
        CATCHING_UP_STATE, // getting the current ledger from the network
        SYNCED_STATE, // we are on the current ledger and are keeping up with
                      // deltas
        NUM_STATE
    };

  private:

    State mState;
    VirtualClock& mVirtualClock;
    Config const& mConfig;

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

    PeerMaster mPeerMaster;
    LedgerMaster mLedgerMaster;
    TxHerder mTxHerder;
    FBAMaster mFBAMaster;
    BucketList mBucketList;
    HistoryMaster mHistoryMaster;
    ProcessMaster mProcessMaster;
    CommandHandler mCommandHandler;
    Database mDatabase;

    std::vector<std::thread> mWorkerThreads;

    asio::signal_set mStopSignals;

    size_t mRealTimerCancelCallbacks;
    void runWorkerThread(unsigned i);
    void scheduleRealTimerFromNextVirtualEvent();
    size_t advanceVirtualTimeToRealTime();
    void realTimerTick();

  public:
    Application(VirtualClock& clock, Config const& config);
    ~Application();

    void enableRealTimer();
    void disableRealTimer();
    size_t crank(bool block=true);

    Config const&
    getConfig()
    {
        return mConfig;
    }

    State
    getState()
    {
        return mState;
    }

    void
    setState(State s)
    {
        mState = s;
    }

    VirtualClock&
    getClock()
    {
        return mVirtualClock;
    }
    LedgerGateway&
    getLedgerGateway()
    {
        return mLedgerMaster;
    }
    FBAGateway&
    getFBAGateway()
    {
        return mFBAMaster;
    }
    CLFGateway&
    getCLFGateway()
    {
        return mBucketList;
    }
    HistoryGateway&
    getHistoryGateway()
    {
        return mHistoryMaster;
    }
    ProcessGateway&
    getProcessGateway()
    {
        return mProcessMaster;
    }
    TxHerderGateway&
    getTxHerderGateway()
    {
        return mTxHerder;
    }
    OverlayGateway&
    getOverlayGateway()
    {
        return mPeerMaster;
    }
    PeerMaster&
    getPeerMaster()
    {
        return mPeerMaster;
    }
    Database&
    getDatabase()
    {
        return mDatabase;
    }

    asio::io_service&
    getMainIOService()
    {
        return mMainIOService;
    }
    asio::io_service&
    getWorkerIOService()
    {
        return mWorkerIOService;
    }

    void start();

    // Stops the io_services, which should cause the threads to exit
    // once they finish running any work-in-progress. If you want a
    // more abrupt exit than this, call exit() and hope for the best.
    void gracefulStop();

    // Wait-on and join all the threads this application started; should
    // only return when there is no more work to do or someone has
    // force-stopped the io_services. Application can be safely destroyed
    // after this returns.
    void joinAllThreads();
};
}

#endif
