#pragma once
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "lib/util/cpptoml.h"
#include "overlay/StellarXDR.h"
#include "util/SecretValue.h"
#include "util/Timer.h"
#include "util/UnorderedMap.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#define DEFAULT_PEER_PORT 11625

namespace stellar
{
extern bool gIsProductionNetwork;

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

// This struct holds information necessary to compute the weight of a validator
// for leader election
struct ValidatorWeightConfig
{
    // Mapping from node ids to info about each validator
    UnorderedMap<NodeID, ValidatorEntry> mValidatorEntries;

    // Mapping from org names to the number of validators in that org
    UnorderedMap<std::string, uint64> mHomeDomainSizes;

    // Weights for each quality level
    UnorderedMap<ValidatorQuality, uint64> mQualityWeights;
};

class Config : public std::enable_shared_from_this<Config>
{

    void validateConfig(ValidationThresholdLevels thresholdLevel);
    void loadQset(std::shared_ptr<cpptoml::table> group, SCPQuorumSet& qset,
                  uint32 level);

    void processConfig(std::shared_ptr<cpptoml::table>);

    void parseNodeID(std::string configStr, PublicKey& retKey);
    void parseNodeID(std::string configStr, PublicKey& retKey, SecretKey& sKey,
                     bool isSeed);

    void parseNodeIDsIntoSet(std::shared_ptr<cpptoml::table> t,
                             std::string const& configStr,
                             std::set<PublicKey>& keySet);

    std::string expandNodeID(std::string const& s) const;
    void addValidatorName(std::string const& pubKeyStr,
                          std::string const& name);
    void addHistoryArchive(std::string const& name, std::string const& get,
                           std::string const& put, std::string const& mkdir);

    std::string toString(ValidatorQuality q) const;
    ValidatorQuality parseQuality(std::string const& q) const;

    std::vector<ValidatorEntry> parseValidators(
        std::shared_ptr<cpptoml::base> validators,
        UnorderedMap<std::string, ValidatorQuality> const& domainQualityMap);

    UnorderedMap<std::string, ValidatorQuality>
    parseDomainsQuality(std::shared_ptr<cpptoml::base> domainsQuality);

    static SCPQuorumSet
    generateQuorumSetHelper(std::vector<ValidatorEntry>::const_iterator begin,
                            std::vector<ValidatorEntry>::const_iterator end,
                            ValidatorQuality curQuality);

    static SCPQuorumSet
    generateQuorumSet(std::vector<ValidatorEntry> const& validators);

    void addSelfToValidators(
        std::vector<ValidatorEntry>& validators,
        UnorderedMap<std::string, ValidatorQuality> const& domainQualityMap);

    void verifyHistoryValidatorsBlocking(
        std::vector<ValidatorEntry> const& validators);

    void verifyLoadGenOpCountForTestingConfigs();

    std::vector<std::chrono::microseconds> mOpApplySleepTimeForTesting;

    template <typename T>
    void verifyLoadGenDistribution(std::vector<T> const& values,
                                   std::vector<uint32_t> const& distribution,
                                   std::string const& valuesName,
                                   std::string const& distributionName);

    // Sets VALIDATOR_WEIGHT_CONFIG based on the content of `validators`. No-op
    // if this node is not a validator.
    void
    setValidatorWeightConfig(std::vector<ValidatorEntry> const& validators);

  public:
    static const uint32 CURRENT_LEDGER_PROTOCOL_VERSION;

    typedef std::shared_ptr<Config> pointer;

    // These test modes should be used in the following contexts:
    // 1. TESTDB_DEFAULT / TESTDB_BUCKET_DB_VOLATILE: provides the most
    //    comprehensive end-to-end test, but does not support arbitrary ledger
    //    state writes via ltx root commits. All ledger state changes must occur
    //    via applying valid TXs or manually adding entries to the BucketList.
    //    BucketList state is not preserved over restarts. If this mode can be
    //    used, it should be.
    // 2. TESTDB_IN_MEMORY: allows arbitrary ledger state writes via
    //    ltx root commits. Suitable for
    //    tests that required writes to the ledger state that cannot be achieved
    //    via valid TX application, such as testing invalid TX error codes or
    //    low level op testing.
    // 3. TESTDB_POSTGRESQL: Should only be used to test POSTGRESQL specific
    //    database operations.
    // 4. TESTDB_BUCKET_DB_PERSISTENT: Same as TESTDB_BUCKET_DB_VOLATILE, but
    //    persists the BucketList and SQL DB over restart. This mode is very
    //    slow and should only be used for testing restart behavior or some low
    //    level BucketList features or for testing SQLite DB specific behavior.
    enum TestDbMode
    {
        TESTDB_DEFAULT,
        TESTDB_IN_MEMORY,
#ifdef USE_POSTGRES
        TESTDB_POSTGRESQL,
#endif
        TESTDB_BUCKET_DB_VOLATILE,
        TESTDB_BUCKET_DB_PERSISTENT,
        TESTDB_MODES
    };

    // application config

    // The default way stellar-core starts is to load the state from disk and
    // start a consensus round (if node is validating), then maybe trigger
    // catchup. If you need different behavior you need to use new-db or
    // --wait-for-consensus option which sets the following flag to false:

    // SCP will start running immediately using the current local state to
    // participate in consensus
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

#ifdef BUILD_TESTS
    // Mode for "accelerated" catchup. If set to true, the node will skip
    // application of failed transactions and will not verify signatures of
    // successful transactions.
    bool CATCHUP_SKIP_KNOWN_RESULTS_FOR_TESTING;
#endif // BUILD_TESTS

    // Interval between automatic maintenance executions
    std::chrono::seconds AUTOMATIC_MAINTENANCE_PERIOD;

    // Number of unneeded rows in each table that will be removed during one
    // maintenance run
    uint32_t AUTOMATIC_MAINTENANCE_COUNT;

    // Interval between automatic invocations of self-check.
    std::chrono::seconds AUTOMATIC_SELF_CHECK_PERIOD;

    // A config parameter that enables synthetic load generation on demand,
    // using the `generateload` runtime command (see CommandHandler.cpp). This
    // option only exists for stress-testing and should not be enabled in
    // production networks.
    bool ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING;

    // A temporary config paramter that when enabled causes the protocol
    // upgrades to also update the Soroban cost calibration. This will result
    // in loadgen reflecting more accurate costs and match the real network.
    // This also makes the node unable to catchup with the real traffic and
    // thus should only be used in tests that only use loadgen.
    // This can be defaulted to 'true' and then removed once the stable Core
    // build supports the new logic.
    bool UPDATE_SOROBAN_COSTS_DURING_PROTOCOL_UPGRADE_FOR_TESTING;

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
    // within LiveBucket::fresh; this option exists only for calculating
    // adjustments to the expected count of merges when stopping and resuming
    // merges, and should be false in all normal cases.
    bool ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING;

    // A config parameter that skips adjustment of target outbound connections
    // based on the inbound connections.
    bool ARTIFICIALLY_SKIP_CONNECTION_ADJUSTMENT_FOR_TESTING;

    // A config parameter that forces replay to use the newest bucket logic;
    // this implicitly means that replay will _not_ check bucket-list hashes
    // along the way, but rather will use the stated hashes from ledger headers
    // _in place of_ the real bucket list hash. This should only be enabled when
    // testing since it completely defeats the state-integrity checking of the
    // system.
    bool ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING;

    // A config parameter that forces a delay in state rebuild via buckets in
    // captive core. This is useful for testing how stellar-core buffers ledgers
    // during captive core fast restart.
    std::chrono::seconds ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING;

    // A config parameter that forces stellar-core to sleep every time a task is
    // picked up from the scheduler. This is useful to imitate a "slow" node.
    // This config should only be enabled when testing.
    std::chrono::microseconds ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING;

    // A config parameter that forces stellar-core to sleep every time it closes
    // a ledger if order to simulate slow application. This config should only
    // be enabled when testing.
    std::chrono::milliseconds ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING;

    // Timeout before publishing externalized values to archive
    std::chrono::seconds PUBLISH_TO_ARCHIVE_DELAY;

    // Config parameters that force transaction application during ledger
    // close to sleep for a certain amount of time.
    // The probability that it sleeps for
    // OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING[i] microseconds is
    // OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING[i] divided by
    // (OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING[0] +
    // OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING[1] + ...) for each i. These
    // options are only for consensus and overlay simulation testing. These two
    // must be used together.
    std::vector<std::chrono::microseconds>
        OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING;
    std::vector<uint32> OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING;

    // Config parameters that LoadGen uses to decide the number of operations
    // to include in each transaction and its distribution.
    // The probability that transactions will contain COUNT[i] operations
    // is DISTRIBUTION[i] / (DISTRIBUTION[0] + DISTRIBUTION[1] + ...) for each
    // i.
    std::vector<unsigned short> LOADGEN_OP_COUNT_FOR_TESTING;
    std::vector<uint32> LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING;

    // Size of wasm blobs for SOROBAN_UPLOAD and MIX_CLASSIC_SOROBAN loadgen
    // modes
    std::vector<uint32_t> LOADGEN_WASM_BYTES_FOR_TESTING;
    std::vector<uint32_t> LOADGEN_WASM_BYTES_DISTRIBUTION_FOR_TESTING;

    // Number of data entries for SOROBAN_INVOKE and MIX_CLASSIC_SOROBAN
    // loadgen modes
    std::vector<uint32_t> LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING;
    std::vector<uint32_t> LOADGEN_NUM_DATA_ENTRIES_DISTRIBUTION_FOR_TESTING;

    // Total kilobytes of reads and writes per transaction for SOROBAN_INVOKE
    // and MIX_CLASSIC_SOROBAN loadgen modes.
    std::vector<uint32_t> LOADGEN_IO_KILOBYTES_FOR_TESTING;
    std::vector<uint32_t> LOADGEN_IO_KILOBYTES_DISTRIBUTION_FOR_TESTING;

    // Transaction size in bytes for SOROBAN_INVOKE and MIX_CLASSIC_SOROBAN
    // loadgen modes
    std::vector<uint32_t> LOADGEN_TX_SIZE_BYTES_FOR_TESTING;
    std::vector<uint32_t> LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING;

    // Instructions per transaction for SOROBAN_INVOKE and MIX_CLASSIC_SOROBAN
    // loadgen modes
    // Also used for configuring apply-load command.
    std::vector<uint32_t> LOADGEN_INSTRUCTIONS_FOR_TESTING;
    std::vector<uint32_t> LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING;

    // apply-load-specific configuration parameters:
    // Size of the synthetic contract data entries used in apply-load.
    // Currently we generate entries of the equal size for more precise
    // control over the modelled instructions.
    uint32_t APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING = 0;

    // The parameters below control the synthetic bucket list generation in
    // apply-load.

    // Number of ledgers to simulate in apply-load. The more ledgers there are,
    // the more bucket list levels will be populated.
    uint32_t APPLY_LOAD_BL_SIMULATED_LEDGERS = 1000;
    // Write a batch of entries every that many ledgers.
    uint32_t APPLY_LOAD_BL_WRITE_FREQUENCY = 1000;
    // Number of entries to write in every batch.
    uint32_t APPLY_LOAD_BL_BATCH_SIZE = 1000;
    // The final `APPLY_LOAD_BL_LAST_BATCH_LEDGERS` of synthetic load will each
    // have `APPLY_LOAD_BL_LAST_BATCH_SIZE` entries in order to populate the
    // lowest BL levels.
    uint32_t APPLY_LOAD_BL_LAST_BATCH_LEDGERS = 300;
    // Number of entries to write in every ledger of
    // `APPLY_LOAD_BL_LAST_BATCH_LEDGERS`.
    uint32_t APPLY_LOAD_BL_LAST_BATCH_SIZE = 100;

    // The APPLY_LOAD_* parameters below are for initializing Soroban
    // settings before applying the benchmark transactions.
    uint32_t APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS = 0;
    uint32_t APPLY_LOAD_TX_MAX_INSTRUCTIONS = 0;

    uint32_t APPLY_LOAD_LEDGER_MAX_READ_LEDGER_ENTRIES = 0;
    uint32_t APPLY_LOAD_TX_MAX_READ_LEDGER_ENTRIES = 0;

    uint32_t APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES = 0;
    uint32_t APPLY_LOAD_TX_MAX_WRITE_LEDGER_ENTRIES = 0;

    uint32_t APPLY_LOAD_LEDGER_MAX_READ_BYTES = 0;
    uint32_t APPLY_LOAD_TX_MAX_READ_BYTES = 0;

    uint32_t APPLY_LOAD_LEDGER_MAX_WRITE_BYTES = 0;
    uint32_t APPLY_LOAD_TX_MAX_WRITE_BYTES = 0;

    uint32_t APPLY_LOAD_MAX_TX_SIZE_BYTES = 0;
    uint32_t APPLY_LOAD_MAX_LEDGER_TX_SIZE_BYTES = 0;

    uint32_t APPLY_LOAD_MAX_CONTRACT_EVENT_SIZE_BYTES = 0;
    uint32_t APPLY_LOAD_MAX_TX_COUNT = 0;

    // Number of read-only and read-write entries in the apply-load
    // transactions. Every entry will have
    // `APPLY_LOAD_DATA_ENTRY_SIZE_FOR_TESTING` size.
    std::vector<uint32_t> APPLY_LOAD_NUM_RO_ENTRIES_FOR_TESTING;
    std::vector<uint32_t> APPLY_LOAD_NUM_RO_ENTRIES_DISTRIBUTION_FOR_TESTING;
    std::vector<uint32_t> APPLY_LOAD_NUM_RW_ENTRIES_FOR_TESTING;
    std::vector<uint32_t> APPLY_LOAD_NUM_RW_ENTRIES_DISTRIBUTION_FOR_TESTING;

    // Number of events to generate in the apply-load transactions.
    std::vector<uint32_t> APPLY_LOAD_EVENT_COUNT_FOR_TESTING;
    std::vector<uint32_t> APPLY_LOAD_EVENT_COUNT_DISTRIBUTION_FOR_TESTING;

    // Waits for merges to complete before applying transactions during catchup
    bool CATCHUP_WAIT_MERGES_TX_APPLY_FOR_TESTING;

    // Overrides the maximum survey phase duration for both the collecting and
    // reporting phase to the specified value. Performs no override if set to 0.
    // Do not use in production. This option is ignored in builds without tests
    // enabled.
    std::chrono::minutes ARTIFICIALLY_SET_SURVEY_PHASE_DURATION_FOR_TESTING;

    // A config parameter that controls how many messages from a particular peer
    // core can process simultaneously. If core is at capacity, it temporarily
    // stops reading from a peer until it completes processing of at least one
    // in-flight message
    uint32_t PEER_READING_CAPACITY;

    // A config parameter that controls how many flood messages (tx or SCP) from
    // a particular peer core can process simultaneously
    uint32_t PEER_FLOOD_READING_CAPACITY;

    // When flow control is enabled, peer asks for more data every time it
    // processes `FLOW_CONTROL_SEND_MORE_BATCH_SIZE` messages
    uint32_t FLOW_CONTROL_SEND_MORE_BATCH_SIZE;

    // PEER_FLOOD_READING_CAPACITY_BYTES (C): This is the initial credit
    // given to the sender. It is the maximum number of bytes that the
    // sender can transmit to the receiver before it needs to wait for
    // an acknowledgement from the receiver. It represents the initial
    // 'capacity' of the connection.

    // MAX_CLASSIC_TX_SIZE_BYTES (M): This is the maximum size, in bytes, of
    // a single message that can be sent by the sender. The sender can send
    // messages of any size up to this limit, provided it has enough credit.

    // FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES (A): This is the number of
    // bytes that the receiver must process before it sends an
    // acknowledgement back to the sender. The acknowledgement also serves
    // to replenish the sender's credit by this amount, enabling it to send
    // more data.

    // The relationship between these three parameters should satisfy: C - A
    // >= M. This ensures that the sender can always continue sending
    // messages until it receives an acknowledgement for the previous data,
    // thus preventing the system from getting stuck.

    // Start with initial PEER_FLOOD_READING_CAPACITY_BYTES (C) credit
    // Sender (C) -------- M1 bytes ----------> Receiver
    //          \-- C-M1 --/

    // Receiver processes received bytes and once
    // FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES (A) or more is processed, an
    // acknowledgement is sent, which replenishes the sender's credit
    // Sender (C-M1+A) <-- A bytes ---------- Receiver
    //             \--- (C-M1+A)-M2 --->/

    // Note:  M1, M2... are message sizes such that M <=
    // MAX_CLASSIC_TX_SIZE_BYTES
    uint32_t PEER_FLOOD_READING_CAPACITY_BYTES;
    uint32_t FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;

    // Byte limit for outbound transaction queue.
    uint32_t OUTBOUND_TX_QUEUE_BYTE_LIMIT;

    // A config parameter that allows a node to generate buckets. This should
    // be set to `false` only for testing purposes.
    bool MODE_ENABLES_BUCKETLIST;

    // A config parameter that can be set to true (in a captive-core
    // configuration) to delay emitting metadata by one ledger.
    bool EXPERIMENTAL_PRECAUTION_DELAY_META;

    // Page size exponent used by BucketIndex when indexing ranges of
    // BucketEntry's. If set to 0, BucketEntry's are individually indexed.
    // Otherwise, pageSize ==
    // 2^BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT.
    size_t BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;

    // Size, in MB, determining whether a bucket should have an individual
    // key index or a key range index. If bucket size is below this value, range
    // based index will be used. If set to 0, all buckets are range indexed. If
    // index page size == 0, value ingnored and all buckets have individual key
    // index.
    size_t BUCKETLIST_DB_INDEX_CUTOFF;

    // Enable parallel processing of overlay operations (experimental)
    bool BACKGROUND_OVERLAY_PROCESSING;

    // Enable parallel block application (experimental)
    bool EXPERIMENTAL_PARALLEL_LEDGER_CLOSE;

    // When set to true, BucketListDB indexes are persisted on-disk so that the
    // BucketList does not need to be reindexed on startup. Defaults to true.
    // This should only be set to false for testing purposes
    // Validators do not currently support persisted indexes. If
    // NODE_IS_VALIDATOR=true, this value is ingnored and indexes are never
    // persisted.
    bool BUCKETLIST_DB_PERSIST_INDEX;

    // A config parameter that stores historical data, such as transactions,
    // fees, and scp history in the database
    bool MODE_STORES_HISTORY_MISC;

    // A config parameter that stores ledger headers in the database
    bool MODE_STORES_HISTORY_LEDGERHEADERS;

    // A config parameter that controls whether core automatically catches up
    // when it has buffered enough input; if false an out-of-sync node will
    // remain out-of-sync, buffering ledgers from the network in memory until
    // it is halted.
    bool MODE_DOES_CATCHUP;

    // A config parameter that controls whether the application starts the
    // overlay on startup, or waits for a later startup after performing some
    // other pre-overlay-start operations (eg. offline catchup).
    bool MODE_AUTO_STARTS_OVERLAY;

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

    // Number of most recent ledgers to remember. Defaults to 12, or
    // approximately ~1 min of network activity.
    uint32 MAX_SLOTS_TO_REMEMBER;

    // A string specifying a stream to write fine-grained metadata to for each
    // ledger close while running. This will be opened at startup and
    // synchronously streamed-to during both catchup and live ledger-closing.
    //
    // Streams may be specified either as a pathname (typically a named FIFO on
    // POSIX or a named pipe on Windows, though plain files also work) or a
    // string of the form "fd:N" for some integer N which, on POSIX, specifies
    // the existing open file descriptor N inherited by the process (for example
    // to write to an anonymous pipe).
    //
    // As a further safety check, this option is mutually exclusive with
    // NODE_IS_VALIDATOR, as its typical use writing to a pipe with a reader
    // process on the other end introduces a potentially-unbounded synchronous
    // delay in closing a ledger, and should not be used on a node participating
    // in consensus, only a passive "watcher" node.
    std::string METADATA_OUTPUT_STREAM;

    // Number of ledgers worth of transaction metadata to preserve on disk for
    // debugging purposes. These records are automatically maintained and
    // rotated during processing, and are helpful for recovery in case of a
    // serious error; they should only be reduced or disabled if disk space is
    // at a premium.
    uint32_t METADATA_DEBUG_LEDGERS;

    // maximum protocol version supported by the application, can be overridden
    // in tests
    uint32_t LEDGER_PROTOCOL_VERSION;
    // min ledger version for which internal errors are reported with high
    // severity
    uint32_t LEDGER_PROTOCOL_MIN_VERSION_INTERNAL_ERROR_REPORT;
    VirtualClock::system_time_point TESTING_UPGRADE_DATETIME;

    // maximum allowed drift for close time when joining the network for the
    // first time
    time_t MAXIMUM_LEDGER_CLOSETIME_DRIFT;

    // Maximum allowed number of DEX-related operations in the transaction set.
    //
    // Transaction is considered to have DEX-related operations if it has path
    // payments or manage offer operations.
    //
    // Setting this to non-nullopt value results in the following:
    // - The node will limit the number of accepted DEX-related transactions
    //   proportional to `MAX_DEX_TX_OPERATIONS_IN_TX_SET / maxTxSetSize`
    //   (ledger header parameter).
    // - The node will broadcast less DEX-related transactions according to the
    //   proportion above.
    // - Starting from protocol 20 the node will nominate TX sets that respect
    //   this limit and potentially have DEX-related transactions surge-priced
    //   against each other.
    std::optional<uint32_t> MAX_DEX_TX_OPERATIONS_IN_TX_SET;

    // note: all versions in the range
    // [OVERLAY_PROTOCOL_MIN_VERSION, OVERLAY_PROTOCOL_VERSION] must be handled
    uint32_t OVERLAY_PROTOCOL_MIN_VERSION; // min overlay version understood
    uint32_t OVERLAY_PROTOCOL_VERSION;     // max overlay version understood
    std::string VERSION_STR;
    std::string LOG_FILE_PATH;
    bool LOG_COLOR;
    std::string BUCKET_DIR_PATH;

    // Ledger protocol version for testing purposes. Defaulted to
    // LEDGER_PROTOCOL_VERSION. Used in the following scenarios: 1. to specify
    // the genesis ledger version (only when USE_CONFIG_FOR_GENESIS is true) 2.
    // as the protocol version for Upgrades.
    uint32_t TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION;
    uint32_t TESTING_UPGRADE_DESIRED_FEE; // in stroops
    uint32_t TESTING_UPGRADE_RESERVE;     // in stroops
    uint32_t TESTING_UPGRADE_MAX_TX_SET_SIZE;
    uint32_t TESTING_UPGRADE_FLAGS;

    unsigned short HTTP_PORT; // what port to listen for commands
    unsigned short
        HTTP_QUERY_PORT;   // what port to listen for RPC related commands
    bool PUBLIC_HTTP_PORT; // if you accept commands from not localhost
    int HTTP_MAX_CLIENT;   // maximum number of http clients, i.e backlog
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
    int MAX_BATCH_WRITE_COUNT;
    int MAX_BATCH_WRITE_BYTES;
    double FLOOD_OP_RATE_PER_LEDGER;
    int FLOOD_TX_PERIOD_MS;
    double FLOOD_SOROBAN_RATE_PER_LEDGER;
    int FLOOD_SOROBAN_TX_PERIOD_MS;
    int32_t FLOOD_ARB_TX_BASE_ALLOWANCE;
    double FLOOD_ARB_TX_DAMPING_FACTOR;

    std::chrono::milliseconds FLOOD_DEMAND_PERIOD_MS;
    std::chrono::milliseconds FLOOD_ADVERT_PERIOD_MS;
    std::chrono::milliseconds FLOOD_DEMAND_BACKOFF_DELAY_MS;
    static constexpr size_t const POSSIBLY_PREFERRED_EXTRA = 2;
    static constexpr size_t const REALLY_DEAD_NUM_FAILURES_CUTOFF = 120;

    // survey config
    std::set<PublicKey> SURVEYOR_KEYS;

    // Peers we will always try to stay connected to
    std::vector<std::string> PREFERRED_PEERS;
    std::vector<std::string> KNOWN_PEERS;

    // Preference can also be expressed by peer pubkey
    std::set<PublicKey> PREFERRED_PEER_KEYS;

    // Whether to exclude peers that are not preferred.
    bool PREFERRED_PEERS_ONLY;

    // thread-management config
    int WORKER_THREADS;

    // Number of threads to serve query commands
    int QUERY_THREAD_POOL_SIZE;

    // Number of ledger snapshots to maintain for querying
    uint32_t QUERY_SNAPSHOT_LEDGERS;

    // process-management config
    size_t MAX_CONCURRENT_SUBPROCESSES;

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

    // Information necessary to compute the weight of a validator for leader
    // election. Nullopt if this node is not a validator, or if this node is
    // using manual quorum set configuration.
    std::optional<ValidatorWeightConfig> VALIDATOR_WEIGHT_CONFIG;

    // Revert to the old, application-agnostic nomination weight function for
    // SCP leader election.
    bool FORCE_OLD_STYLE_LEADER_ELECTION;

    // History config
    std::map<std::string, HistoryArchiveConfiguration> HISTORY;

    // Database config
    SecretValue DATABASE;

    std::vector<std::string> COMMANDS;
    std::vector<std::string> REPORT_METRICS;

    // Data layer cache configuration
    // - ENTRY_CACHE_SIZE controls the maximum number of LedgerEntry objects
    //   that will be stored in the cache
    size_t ENTRY_CACHE_SIZE;

    // Data layer prefetcher configuration
    // - PREFETCH_BATCH_SIZE determines how many records we'll prefetch per
    // SQL load. Note that it should be significantly smaller than size of
    // the entry cache
    size_t PREFETCH_BATCH_SIZE;

    // If set to true, the application will halt when an internal error is
    // encountered during applying a transaction. Otherwise, the
    // txINTERNAL_ERROR transaction is created but not applied.
    // Enabling this is useful for debugging the transaction errors caused by
    // the core's internal errors via catching them early.
    // The default value is false.
    bool HALT_ON_INTERNAL_TRANSACTION_ERROR;

    // If set to true, additional diagnostic Soroban events that are not part
    // of the protocol will be generated while applying Soroban transactions.
    // These events will be put into a list in the non-hashed portion of the
    // meta, and this list will contain all events so ordering can be
    // maintained between all events. The default value is false, and this
    // should not be enabled on validators.
    bool ENABLE_SOROBAN_DIAGNOSTIC_EVENTS;

    // If set to true, attach a small diagnostics message in the format of
    // Soroban diagnostic event to some transaction submission errors (mainly,
    // `txSOROBAN_INVALID` errors). The diagnostics message is guaranteed to be
    // small and independent of the user input, so this can be safely enabled
    // on validators that accept transactions.
    bool ENABLE_DIAGNOSTICS_FOR_TX_SUBMISSION;

    // Override the initial hardcoded MINIMUM_PERSISTENT_ENTRY_LIFETIME
    // for testing.
    uint32_t TESTING_MINIMUM_PERSISTENT_ENTRY_LIFETIME;

    // Increase all initial max limits to higher values for testing
    bool TESTING_SOROBAN_HIGH_LIMIT_OVERRIDE;

    // Override eviction parameters for testing. If
    // OVERRIDE_EVICTION_PARAMS_FOR_TESTING is true, all the eviction TESTING_*
    // parameters will be used instead of the default values.
    bool OVERRIDE_EVICTION_PARAMS_FOR_TESTING;
    uint32_t TESTING_EVICTION_SCAN_SIZE;
    uint32_t TESTING_STARTING_EVICTION_SCAN_LEVEL;
    uint32_t TESTING_MAX_ENTRIES_TO_ARCHIVE;

    bool EMIT_SOROBAN_TRANSACTION_META_EXT_V1;
    bool EMIT_LEDGER_CLOSE_META_EXT_V1;

#ifdef BUILD_TESTS
    // If set to true, the application will be aware this run is for a test
    // case.  This is used right now in the signal handler to exit() instead of
    // doing a graceful shutdown
    bool TEST_CASES_ENABLED;

    // A config parameter that uses a never-committing ledger. This means that
    // all ledger entries, except for offers, will be kept in memory, and not
    // persisted to DB. Since offers are backed by SQL and not BucketListDB,
    // offers are still commited to the SQL DB even when this mode is enabled.
    // Should only be used for testing.
    bool MODE_USES_IN_MEMORY_LEDGER;

    // Set QUORUM_SET using automatic quorum set configuration based on
    // `validators`.
    void
    generateQuorumSetForTesting(std::vector<ValidatorEntry> const& validators);
#endif

#ifdef BEST_OFFER_DEBUGGING
    bool BEST_OFFER_DEBUGGING_ENABLED;
#endif

    // Any transaction that reaches the TransactionQueue will be rejected if it
    // contains an operation in this list.
    std::vector<OperationType> EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE;

    Config();

    void load(std::string const& filename);
    void load(std::istream& in);

    // fixes values of connection-relates settings
    void adjust();

    std::string toShortString(NodeID const& pk) const;

    // fullKey true => returns full StrKey corresponding to pk
    //  otherwise, returns alias or shortString equivalent
    std::string toStrKey(PublicKey const& pk, bool fullKey) const;

    bool resolveNodeID(std::string const& s, PublicKey& retKey) const;

    std::chrono::seconds getExpectedLedgerCloseTime() const;

    bool modeDoesCatchupWithBucketList() const;
    bool isPersistingBucketListDBIndexes() const;
    bool modeStoresAllHistory() const;
    bool modeStoresAnyHistory() const;
    void logBasicInfo() const;
    bool parallelLedgerClose() const;
    void setNoListen();
    void setNoPublish();

    // function to stringify a quorum set
    std::string toString(SCPQuorumSet const& qset);

    // A special name to be used for stdin in stead of a file name in command
    // line arguments.
    static std::string const STDIN_SPECIAL_NAME;

    void processOpApplySleepTimeForTestingConfigs();

    std::chrono::seconds HISTOGRAM_WINDOW_SIZE;
};
}
