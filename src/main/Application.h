#ifndef __APPLICATION__
#define __APPLICATION__


#include "fba/FBAMaster.h"
#include "ledger/LedgerMaster.h"
#include "main/Config.h"
#include "txherder/TxHerder.h"
#include "overlay/OverlayGateway.h"
#include "overlay/PeerMaster.h"

/*
 * State of a single instance of the application.
 *
 * Multiple instances may exist in the same process, eg. for the sake of
 * testing by simulating a network of applications.
 *
 * Owns two asio::io_services, one "main" (usually driven by a main thread) and
 * one "worker" (usually driven by a pool of #NCORE worker threads).  The main
 * thread/io_service has the run of the application and responds to the majority
 * of (small, sequential, consensus-related) network requests. The worker
 * threads/io_service are for long-running, self-contained helper jobs such as
 * bulk transfers and hashing. They should not touch anything outside their own
 * job-state (i.e. in a closure) and should post results back to the main
 * io_service when complete.
 *
 * If the application is constructed in "single step" mode
 * (cfg.SINGLE_STEP_MODE) then no threads are created and the owner of the
 * Application has to manually step the event loops forware using
 * app.getMainIOService().run_one() or .poll_one() or the like. This mode is
 * useful in running multiple Applications inside a test, for simulating a
 * specific, deterministic communication pattern between nodes.
 */

namespace stellar
{
    class Application : public enable_shared_from_this<Application>
    {
    public:

        enum
        {
            BOOTING_STATE,      // loading last known ledger from disk
            CONNECTING_STATE,   // trying to connect to other peers
            CONNECTED_STATE,    // connected to other peers and receiving validations
            CATCHING_UP_STATE,  // getting the current ledger from the network
            SYNCED_STATE,       // we are on the current ledger and are keeping up with deltas
            NUM_STATE
        };

        int mState;
        Config const& mConfig;

    private:

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

        PeerMaster mPeerMaster;
        LedgerMaster mLedgerMaster;
        TxHerder mTxHerder;
        FBAMaster mFBAMaster;

        std::unique_ptr<std::thread> mMainThread;
        std::vector<std::thread> mWorkerThreads;

        asio::signal_set mStopSignals;

        void runMainThread();
        void runWorkerThread(unsigned i);

    public:

        Application(Config const& config);

        LedgerGateway& getLedgerGateway(){ return mLedgerMaster; }
        FBAGateway& getFBAGateway(){ return mFBAMaster; }
        //CLFGateway& getCLFGateway();
        //HistoryGateway& getHistoryGateway();
        TxHerderGateway& getTxHerderGateway(){ return mTxHerder; }
        OverlayGateway& getOverlayGateway() { return mPeerMaster; }
        PeerMaster& getPeerMaster() { return mPeerMaster; }

        asio::io_service& getMainIOService() { return mMainIOService; }
        asio::io_service& getWorkerIOService() { return mWorkerIOService; }

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
