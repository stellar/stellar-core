#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "xdr/Stellar-types.h"
#include <lib/json/json.h>
#include <memory>
#include <string>

namespace asio
{
}
namespace medida
{
class MetricsRegistry;
}

namespace stellar
{

class VirtualClock;
class TmpDirManager;
class LedgerManager;
class BucketManager;
class CatchupManager;
class HistoryManager;
class Maintainer;
class ProcessManager;
class Herder;
class HerderPersistence;
class InvariantManager;
class OverlayManager;
class Database;
class PersistentState;
class LoadGenerator;
class CommandHandler;
class WorkManager;
class BanManager;
class StatusManager;

class Application;
void validateNetworkPassphrase(std::shared_ptr<Application> app);

/*
 * State of a single instance of the stellar-core application.
 *
 * Multiple instances may exist in the same process, eg. for the sake of testing
 * by simulating a network of Applications.
 *
 *
 * Clocks, time and events
 * -----------------------
 *
 * An Application is connected to a VirtualClock, that both manages the
 * Application's view of time and also owns an IO event loop that dispatches
 * events for the main thread. See VirtualClock for details.
 *
 * In order to advance an Application's view of time, as well as dispatch any IO
 * events, timers or callbacks, the associated VirtualClock must be cranked. See
 * VirtualClock::crank().
 *
 * All Applications coordinating on a simulation should be bound to the same
 * VirtualClock, so that their view of time advances from event to event within
 * the collective simulation.
 *
 *
 * Configuration
 * -------------
 *
 * Each Application owns a Config object, which describes any user-controllable
 * configuration variables, including cryptographic keys, network ports, log
 * files, directories and the like. A local copy of the Config object is made on
 * construction of each Application, after which the local copy cannot be
 * further altered; the Application should be destroyed and recreted if any
 * change to configuration is desired.
 *
 *
 * Subsystems
 * ----------
 *
 * Each Application owns a collection of subsystem "manager" objects, typically
 * one per subdirectory in the source tree. For example, the LedgerManager, the
 * OverlayManager, the HistoryManager, etc. Instances of these subsystem objects
 * are generally created in 1:1 correspondence with their owning Application;
 * each Application creates a new LedgerManager for itself, for example.
 *
 * Each subsystem object contains a reference back to its owning Application,
 *and
 * uses this reference to retrieve its Application's associated instance of the
 * other subsystems. So for example an Application's LedgerManager can access
 * that Application's HistoryManager in order to run catchup. Subsystems access
 * one another through virtual interfaces, to afford some degree of support for
 * testing and information hiding.
 *
 *
 * Threading
 * ---------
 *
 * In general, Application expects to run on a single thread -- the main thread
 * -- and most subsystems perform no locking, are not multi-thread
 * safe. Operations with high IO latency are broken into steps and executed
 * piecewise through the VirtualClock's asio::io_service; those with high CPU
 * latency are run on a "worker" thread pool.
 *
 * Each Application owns a secondary "worker" asio::io_service, that queues and
 * dispatches CPU-bound work to a number of worker threads (one per core); these
 * serve only to offload self-contained CPU-bound tasks like hashing from the
 * main thread, and do not generally call back into the Application's owned
 * sub-objects (with a couple exceptions, in the BucketManager and BucketList
 * objects).
 *
 * Completed "worker" tasks typically post their results back to the main
 * thread's io_service (held in the VirtualClock), or else deliver their results
 * to the Application through std::futures or similar standard
 * thread-synchronization primitives.
 */

class Application
{
  public:
    typedef std::shared_ptr<Application> pointer;

    // Running state of an application; different values inhibit or enable
    // certain subsystem responses to IO events, timers etc.
    enum State
    {
        // Loading state from database, not yet active. SCP is inhibited.
        APP_BOOTING_STATE,

        // Out of sync with SCP peers
        APP_ACQUIRING_CONSENSUS_STATE,

        // Connected to other SCP peers

        // in sync with network but ledger subsystem still booting up
        APP_CONNECTED_STANDBY_STATE,

        // some work required to catchup to the consensus ledger
        // ie: downloading from history, applying buckets and replaying
        // transactions
        APP_CATCHING_UP_STATE,

        // In sync with SCP peers, applying transactions. SCP is active,
        APP_SYNCED_STATE,

        // application is shutting down
        APP_STOPPING_STATE,

        APP_NUM_STATE
    };

    virtual ~Application(){};

    virtual void initialize() = 0;

    // Return the time in seconds since the POSIX epoch, according to the
    // VirtualClock this Application is bound to. Convenience method.
    virtual uint64_t timeNow() = 0;

    // Return a reference to the Application-local copy of the Config object
    // that the Application was constructed with.
    virtual Config const& getConfig() = 0;

    // Gets the current execution-state of the Application
    // (derived from the state of other modules
    virtual State getState() const = 0;
    virtual std::string getStateHuman() const = 0;
    virtual bool isStopping() const = 0;

    // Get the external VirtualClock to which this Application is bound.
    virtual VirtualClock& getClock() = 0;

    // Get the registry of metrics owned by this application. Metrics are
    // reported through the administrative HTTP interface, see CommandHandler.
    virtual medida::MetricsRegistry& getMetrics() = 0;

    // Ensure any App-local metrics that are "current state" gauge-like counters
    // reflect the current reality as best as possible.
    virtual void syncOwnMetrics() = 0;

    // Call syncOwnMetrics on self and syncMetrics all objects owned by App.
    virtual void syncAllMetrics() = 0;

    // Clear all metrics
    virtual void clearMetrics(std::string const& domain) = 0;

    // Get references to each of the "subsystem" objects.
    virtual TmpDirManager& getTmpDirManager() = 0;
    virtual LedgerManager& getLedgerManager() = 0;
    virtual BucketManager& getBucketManager() = 0;
    virtual CatchupManager& getCatchupManager() = 0;
    virtual HistoryManager& getHistoryManager() = 0;
    virtual Maintainer& getMaintainer() = 0;
    virtual ProcessManager& getProcessManager() = 0;
    virtual Herder& getHerder() = 0;
    virtual HerderPersistence& getHerderPersistence() = 0;
    virtual InvariantManager& getInvariantManager() = 0;
    virtual OverlayManager& getOverlayManager() = 0;
    virtual Database& getDatabase() const = 0;
    virtual PersistentState& getPersistentState() = 0;
    virtual CommandHandler& getCommandHandler() = 0;
    virtual WorkManager& getWorkManager() = 0;
    virtual BanManager& getBanManager() = 0;
    virtual StatusManager& getStatusManager() = 0;

    // Get the worker IO service, served by background threads. Work posted to
    // this io_service will execute in parallel with the calling thread, so use
    // with caution.
    virtual asio::io_service& getWorkerIOService() = 0;

    // Perform actions necessary to transition from BOOTING_STATE to other
    // states. In particular: either reload or reinitialize the database, and
    // either restart or begin reacquiring SCP consensus (as instructed by
    // Config).
    virtual void start() = 0;

    // Stop the io_services, which should cause the threads to exit once they
    // finish running any work-in-progress.
    virtual void gracefulStop() = 0;

    // Wait-on and join all the threads this application started; should only
    // return when there is no more work to do or someone has force-stopped the
    // worker io_service. Application can be safely destroyed after this
    // returns.
    virtual void joinAllThreads() = 0;

    // If config.MANUAL_MODE=true, force the current ledger to close and return
    // true. Otherwise return false. This method exists only for testing.
    virtual bool manualClose() = 0;

    // If config.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true, generate some load
    // against the current application.
    virtual void generateLoad(bool isCreate, uint32_t nAccounts, uint32_t nTxs,
                              uint32_t txRate, uint32_t batchSize,
                              bool autoRate) = 0;

    // Access the load generator for manual operation.
    virtual LoadGenerator& getLoadGenerator() = 0;

    // Run a consistency check between the database and the bucketlist.
    virtual void checkDB() = 0;

    // Execute any administrative commands written in the Config.COMMANDS
    // variable of the config file. This permits scripting certain actions to
    // occur automatically at startup.
    virtual void applyCfgCommands() = 0;

    // Report, via standard logging, the current state any metrics defined in
    // the Config.REPORT_METRICS (or passed on the command line with --metric)
    virtual void reportCfgMetrics() = 0;

    // Get information about the instance as JSON object
    virtual Json::Value getJsonInfo() = 0;

    // Report information about the instance to standard logging
    virtual void reportInfo() = 0;

    // Returns the hash of the passphrase, used to separate various network
    // instances
    virtual Hash const& getNetworkID() const = 0;

    virtual void newDB() = 0;

    // Factory: create a new Application object bound to `clock`, with a local
    // copy made of `cfg`.
    static pointer create(VirtualClock& clock, Config const& cfg,
                          bool newDB = true);
    template <typename T>
    static std::shared_ptr<T>
    create(VirtualClock& clock, Config const& cfg, bool newDB = true)
    {
        auto ret = std::make_shared<T>(clock, cfg);
        ret->initialize();
        if (newDB || cfg.DATABASE.value == "sqlite3://:memory:")
            ret->newDB();
        validateNetworkPassphrase(ret);

        return ret;
    }

  protected:
    Application()
    {
    }
};
}
