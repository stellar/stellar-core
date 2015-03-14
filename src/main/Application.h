#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>

namespace asio
{
class io_service;
}
namespace medida
{
class MetricsRegistry;
}

namespace stellar
{

class VirtualClock;
class Config;
class TmpDirMaster;
class LedgerGateway;
class LedgerMaster;
class CLFMaster;
class HistoryMaster;
class ProcessGateway;
class HerderGateway;
class OverlayGateway;
class PeerMaster;
class Database;
class PersistentState;

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

class Application
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

    virtual ~Application(){};

    // in seconds
    virtual uint64_t timeNow() = 0;

    virtual Config const& getConfig() = 0;

    virtual State getState() = 0;
    virtual void setState(State) = 0;
    virtual VirtualClock& getClock() = 0;
    virtual medida::MetricsRegistry& getMetrics() = 0;
    virtual TmpDirMaster& getTmpDirMaster() = 0;
    virtual LedgerGateway& getLedgerGateway() = 0;
    virtual LedgerMaster& getLedgerMaster() = 0;
    virtual CLFMaster& getCLFMaster() = 0;
    virtual HistoryMaster& getHistoryMaster() = 0;
    virtual ProcessGateway& getProcessGateway() = 0;
    virtual HerderGateway& getHerderGateway() = 0;
    virtual OverlayGateway& getOverlayGateway() = 0;
    virtual PeerMaster& getPeerMaster() = 0;
    virtual Database& getDatabase() = 0;
    virtual PersistentState& getPersistentState() = 0;

    virtual asio::io_service& getWorkerIOService() = 0;

    virtual void start() = 0;

    // Stops the io_services, which should cause the threads to exit
    // once they finish running any work-in-progress. If you want a
    // more abrupt exit than this, call exit() and hope for the best.
    virtual void gracefulStop() = 0;

    // Wait-on and join all the threads this application started; should
    // only return when there is no more work to do or someone has
    // force-stopped the io_services. Application can be safely destroyed
    // after this returns.
    virtual void joinAllThreads() = 0;

    // only works if config.MANUAL_MODE=true;
    // forces the ledger to close
    virtual bool manualClose() = 0;

    virtual void applyCfgCommands() = 0;

    static pointer create(VirtualClock& clock, Config const& cfg);

  protected:
    Application()
    {
    }
};
}
