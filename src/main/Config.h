#pragma once
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "lib/util/cpptoml.h"
#include "overlay/StellarXDR.h"
#include "util/SecretValue.h"
#include "util/Timer.h"
#include "util/optional.h"

#include <map>
#include <memory>
#include <string>

#define DEFAULT_PEER_PORT 11625

namespace stellar
{
struct HistoryArchiveConfiguration
{
    std::string mName;
    std::string mGetCmd;
    std::string mPutCmd;
    std::string mMkdirCmd;
};

enum class ValidationThresholdLevels : int
{
    SIMPLE_MAJORITY = 0,
    BYZANTINE_FAULT_TOLERANCE = 1,
    ALL_REQUIRED = 2
};

class Config : public std::enable_shared_from_this<Config>
{
    enum class ValidatorQuality : int
    {
        VALIDATOR_LOW_QUALITY = 0,
        VALIDATOR_MED_QUALITY = 1,
        VALIDATOR_HIGH_QUALITY = 2,
        VALIDATOR_CRITICAL_QUALITY = 3
    };

    struct ValidatorEntry
    {
        std::string mName;
        std::string mHomeDomain;
        ValidatorQuality mQuality;
        PublicKey mKey;
        bool mHasHistory;
    };

    void validateConfig(ValidationThresholdLevels thresholdLevel);
    void loadQset(std::shared_ptr<cpptoml::table> group, SCPQuorumSet& qset,
                  int level);

    void processConfig(std::shared_ptr<cpptoml::table>);

    void parseNodeID(std::string configStr, PublicKey& retKey);
    void parseNodeID(std::string configStr, PublicKey& retKey, SecretKey& sKey,
                     bool isSeed);

    std::string expandNodeID(std::string const& s) const;
    void addValidatorName(std::string const& pubKeyStr,
                          std::string const& name);
    void addHistoryArchive(std::string const& name, std::string const& get,
                           std::string const& put, std::string const& mkdir);

    std::string toString(ValidatorQuality q) const;
    ValidatorQuality parseQuality(std::string const& q) const;

    std::vector<ValidatorEntry>
    parseValidators(std::shared_ptr<cpptoml::base> validators,
                    std::unordered_map<std::string, ValidatorQuality> const&
                        domainQualityMap);

    std::unordered_map<std::string, ValidatorQuality>
    parseDomainsQuality(std::shared_ptr<cpptoml::base> domainsQuality);

    static SCPQuorumSet
    generateQuorumSetHelper(std::vector<ValidatorEntry>::const_iterator begin,
                            std::vector<ValidatorEntry>::const_iterator end,
                            ValidatorQuality curQuality);

    static SCPQuorumSet
    generateQuorumSet(std::vector<ValidatorEntry> const& validators);

    void
    addSelfToValidators(std::vector<ValidatorEntry>& validators,
                        std::unordered_map<std::string, ValidatorQuality> const&
                            domainQualityMap);

    void verifyHistoryValidatorsBlocking(
        std::vector<ValidatorEntry> const& validators);

  public:
    static const uint32 CURRENT_LEDGER_PROTOCOL_VERSION;

    typedef std::shared_ptr<Config> pointer;

    enum TestDbMode
    {
        TESTDB_DEFAULT,
        TESTDB_IN_MEMORY_SQLITE,
        TESTDB_ON_DISK_SQLITE,
#ifdef USE_POSTGRES
        TESTDB_POSTGRESQL,
#endif
        TESTDB_MODES
    };

    // application config

    // The default way stellar-core starts is to load the state from disk and
    // catch up to the network before starting SCP. If you need different
    // behavior you need to use new-db or force-scp which sets the following
    // flags:

    // SCP will start running immediately using the current local state to
    // participate in consensus. DO NOT INCLUDE THIS IN A CONFIG FILE
    bool FORCE_SCP;

    // This is a mode for testing. It prevents you from trying to connect to
    // other peers
    bool RUN_STANDALONE;

    // Mode for testing. Ledger will only close when told to over http
    bool MANUAL_CLOSE;

    // Whether to catchup "completely" (replaying all history); default is
    // false,
    // meaning catchup "minimally", using deltas to the most recent snapshot.
    bool CATCHUP_COMPLETE;

    // Number of "recent" ledgers before the current ledger to include in a
    // "minimal" catchup. Default is 0, and if CATCHUP_COMPLETE is set to
    // true, this is ignored.
    //
    // If you want, say, a week of history, set this to 120000.
    uint32_t CATCHUP_RECENT;

    // Interval between automatic maintenance executions
    std::chrono::seconds AUTOMATIC_MAINTENANCE_PERIOD;

    // Number of unneeded rows in each table that will be removed during one
    // maintenance run
    uint32_t AUTOMATIC_MAINTENANCE_COUNT;

    // A config parameter that enables synthetic load generation on demand,
    // using the `generateload` runtime command (see CommandHandler.cpp). This
    // option only exists for stress-testing and should not be enabled in
    // production networks.
    bool ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING;

    // A config parameter that reduces ledger close time to 1s and checkpoint
    // frequency to every 8 ledgers. Do not ever set this in production, as it
    // will make your history archives incompatible with those of anyone else.
    bool ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING;

    // A config parameter to override the close time (in seconds). Do not use
    // in production as it may render the network unstable.
    uint32 ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING;

    // A config parameter that avoids resolving FutureBuckets before writing
    // them to the database's persistent state; this option exists only
    // for stress-testing the ability to resume from an interrupted merge,
    // and should be false in all normal cases.
    bool ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING;

    // A config parameter that avoids counting level 0 merge events and those
    // within Bucket::fresh; this option exists only for calculating adjustments
    // to the expected count of merges when stopping and resuming merges,
    // and should be false in all normal cases.
    bool ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING;

    // A config parameter that forces replay to use the newest bucket logic;
    // this implicitly means that replay will _not_ check bucket-list hashes
    // along the way, but rather will use the stated hashes from ledger headers
    // _in place of_ the real bucket list hash. This should only be enabled when
    // testing since it completely defeats the state-integrity checking of the
    // system.
    bool ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING;

    // A config to allow connections to localhost
    // this should only be enabled when testing as it's a security issue
    bool ALLOW_LOCALHOST_FOR_TESTING;

    // Set to use config file values for genesis ledger
    // not setable in config file - only tests are allowed to do this
    bool USE_CONFIG_FOR_GENESIS;

    // This is the number of failures you want to be able to tolerate.
    // You will need at least 3f+1 nodes in your quorum set.
    // If you don't have enough in your quorum set to tolerate the level you
    //  set here stellar-core won't run.
    int32_t FAILURE_SAFETY;

    // If set to true allows you to specify an unsafe quorum set.
    // Otherwise it won't start if you have your threshold % set too low.
    // You might want to set this if you are running your own network and
    //  aren't concerned with byzantine failures.
    bool UNSAFE_QUORUM;

    // If set to true, bucket GC will not be performed. It can lead to massive
    // disk usage, but it is useful for recovering of nodes.
    bool DISABLE_BUCKET_GC;

    // If set to true, writing an XDR file (a bucket or a checkpoint) will not
    // be followed by an fsync on the file. This in turn means that XDR files
    // (which hold the canonical state of the ledger) may be corrupted if the
    // operating system suddenly crashes or loses power, causing the node to
    // diverge and get stuck on restart, or potentially even publish bad
    // history. This option only exists as an escape hatch if the local
    // filesystem is so unusably slow that you prefer operating without
    // durability guarantees. Do not set it to true unless you're very certain
    // you want to make that trade.
    bool DISABLE_XDR_FSYNC;

    // Set of cursors added at each startup with value '1'.
    std::vector<std::string> KNOWN_CURSORS;

    uint32_t LEDGER_PROTOCOL_VERSION;
    VirtualClock::time_point TESTING_UPGRADE_DATETIME;

    // maximum allowed drift for close time when joining the network for the
    // first time
    uint64 MAXIMUM_LEDGER_CLOSETIME_DRIFT;

    // note: all versions in the range
    // [OVERLAY_PROTOCOL_MIN_VERSION, OVERLAY_PROTOCOL_VERSION] must be handled
    uint32_t OVERLAY_PROTOCOL_MIN_VERSION; // min overlay version understood
    uint32_t OVERLAY_PROTOCOL_VERSION;     // max overlay version understood
    std::string VERSION_STR;
    std::string LOG_FILE_PATH;
    std::string BUCKET_DIR_PATH;
    uint32_t TESTING_UPGRADE_DESIRED_FEE; // in stroops
    uint32_t TESTING_UPGRADE_RESERVE;     // in stroops
    uint32_t TESTING_UPGRADE_MAX_TX_SET_SIZE;
    unsigned short HTTP_PORT; // what port to listen for commands
    bool PUBLIC_HTTP_PORT;    // if you accept commands from not localhost
    int HTTP_MAX_CLIENT;      // maximum number of http clients, i.e backlog
    std::string NETWORK_PASSPHRASE; // identifier for the network

    // overlay config
    unsigned short PEER_PORT;
    unsigned short TARGET_PEER_CONNECTIONS;
    unsigned short MAX_PENDING_CONNECTIONS;
    int MAX_ADDITIONAL_PEER_CONNECTIONS;
    unsigned short MAX_INBOUND_PENDING_CONNECTIONS;
    unsigned short MAX_OUTBOUND_PENDING_CONNECTIONS;
    unsigned short PEER_AUTHENTICATION_TIMEOUT;
    unsigned short PEER_TIMEOUT;
    unsigned short PEER_STRAGGLER_TIMEOUT;
    static constexpr auto const POSSIBLY_PREFERRED_EXTRA = 2;
    static constexpr auto const REALLY_DEAD_NUM_FAILURES_CUTOFF = 120;

    // Peers we will always try to stay connected to
    std::vector<std::string> PREFERRED_PEERS;
    std::vector<std::string> KNOWN_PEERS;

    // Preference can also be expressed by peer pubkey
    std::vector<std::string> PREFERRED_PEER_KEYS;

    // Whether to exclude peers that are not preferred.
    bool PREFERRED_PEERS_ONLY;

    // Percentage, between 0 and 100, of system activity (measured in terms
    // of both event-loop cycles and database time) below-which the system
    // will consider itself "loaded" and attempt to shed load. Set this
    // number low and the system will be tolerant of overloading. Set it
    // high and the system will be intolerant. By default it is 0, meaning
    // totally insensitive to overloading.
    uint32_t MINIMUM_IDLE_PERCENT;

    // thread-management config
    int WORKER_THREADS;

    // process-management config
    int MAX_CONCURRENT_SUBPROCESSES;

    // SCP config
    SecretKey NODE_SEED;
    bool NODE_IS_VALIDATOR;
    stellar::SCPQuorumSet QUORUM_SET;
    // this node's home domain
    std::string NODE_HOME_DOMAIN;

    // Whether to run online quorum intersection checks.
    bool QUORUM_INTERSECTION_CHECKER;

    // Invariants
    std::vector<std::string> INVARIANT_CHECKS;

    std::map<std::string, std::string> VALIDATOR_NAMES;

    // History config
    std::map<std::string, HistoryArchiveConfiguration> HISTORY;

    // Database config
    SecretValue DATABASE;

    std::vector<std::string> COMMANDS;
    std::vector<std::string> REPORT_METRICS;

    // Data layer cache configuration
    // - ENTRY_CACHE_SIZE controls the maximum number of LedgerEntry objects
    //   that will be stored in the cache
    // - BEST_OFFERS_CACHE_SIZE controls the maximum number of Asset pairs that
    //   will be stored in the cache, although many LedgerEntry objects may be
    //   associated with a single Asset pair
    size_t ENTRY_CACHE_SIZE;
    size_t BEST_OFFERS_CACHE_SIZE;

    // Data layer prefetcher configuration
    // - PREFETCH_BATCH_SIZE determines how many records we'll prefetch per
    // SQL load. Note that it should be significantly smaller than size of
    // the entry cache
    size_t PREFETCH_BATCH_SIZE;

    // The version of TransactionMeta that will be generated. Acceptable values
    // are 1 (default) and 2. Set to 2 only if downstream systems have been
    // updated to handle TransactionMetaV2.
    size_t SUPPORTED_META_VERSION;

#ifdef BUILD_TESTS
    // If set to true, the application will be aware this run is for a test
    // case.  This is used right now in the signal handler to exit() instead of
    // doing a graceful shutdown
    bool TEST_CASES_ENABLED;
#endif

    Config();

    void load(std::string const& filename);
    void load(std::istream& in);

    // fixes values of connection-relates settings
    void adjust();

    std::string toShortString(PublicKey const& pk) const;

    // fullKey true => returns full StrKey corresponding to pk
    //  otherwise, returns alias or shortString equivalent
    std::string toStrKey(PublicKey const& pk, bool fullKey) const;

    bool resolveNodeID(std::string const& s, PublicKey& retKey) const;

    std::chrono::seconds getExpectedLedgerCloseTime() const;

    void logBasicInfo();
    void setNoListen();

    // function to stringify a quorum set
    std::string toString(SCPQuorumSet const& qset);
};
}
