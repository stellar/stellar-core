#pragma once
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include <string>
#include <memory>
#include <map>
#include "crypto/SecretKey.h"
#include "lib/util/cpptoml.h"

#define DEFAULT_PEER_PORT 11625

namespace stellar
{
class HistoryArchive;

class Config : public std::enable_shared_from_this<Config>
{
    void validateConfig();
    void loadQset(std::shared_ptr<cpptoml::toml_group> group,
                  SCPQuorumSet& qset, int level);

    void parseNodeID(std::string configStr, PublicKey& retKey);

  public:
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
    // catch
    // up to the network before starting SCP.
    // If you need different behavior you need to use --newdb or --force-scp
    // which sets the following flags:

    // SCP will start running immediately using the current local state to
    // participate in consensus. DO NOT INCLUDE THIS IN A CONFIG FILE
    bool FORCE_SCP;

    // reset the database to the genesis ledger. DO NOT INCLUDE THIS IN
    // A CONFIG FILE
    bool REBUILD_DB;

    // This is a mode for testing. It prevents you from trying to connect to
    // other peers
    bool RUN_STANDALONE;

    // Mode for testing. Ledger will only close when told to over http
    bool MANUAL_CLOSE;

    // Whether to catchup "completely" (replaying all history); default is
    // false,
    // meaning catchup "minimally", using deltas to the most recent snapshot.
    bool CATCHUP_COMPLETE;

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

    // A config to allow connections to localhost
    // this should only be enabled when testing as it's a security issue
    bool ALLOW_LOCALHOST_FOR_TESTING;

    // This is the number of failures you want to be able to tolerate.
    // You will need at least 3f+1 nodes in your quorum set.
    // If you don't have enough in your quorum set to tolerate the level you
    //  set here stellar-core won't run.
    uint32_t FAILURE_SAFETY;

    // If set to true allows you to specify an unsafe quorum set.
    // Otherwise it won't start if you have your threshold % set too low.
    // You might want to set this if you are running your own network and
    //  aren't concerned with byzantine failures.
    bool UNSAFE_QUORUM;

    uint32_t LEDGER_PROTOCOL_VERSION;

    // note: all versions in the range
    // [OVERLAY_PROTOCOL_MIN_VERSION, OVERLAY_PROTOCOL_VERSION] must be handled
    uint32_t OVERLAY_PROTOCOL_MIN_VERSION; // min overlay version understood
    uint32_t OVERLAY_PROTOCOL_VERSION;     // max overlay version understood
    std::string VERSION_STR;
    std::string LOG_FILE_PATH;
    std::string TMP_DIR_PATH;
    std::string BUCKET_DIR_PATH;
    uint32_t DESIRED_BASE_FEE;     // in stroops
    uint32_t DESIRED_BASE_RESERVE; // in stroops
    uint32_t DESIRED_MAX_TX_PER_LEDGER;
    unsigned short HTTP_PORT;       // what port to listen for commands
    bool PUBLIC_HTTP_PORT;          // if you accept commands from not localhost
    std::string NETWORK_PASSPHRASE; // identifier for the network

    // overlay config
    unsigned short PEER_PORT;
    unsigned TARGET_PEER_CONNECTIONS;
    unsigned MAX_PEER_CONNECTIONS;
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

    // process-management config
    size_t MAX_CONCURRENT_SUBPROCESSES;

    // Setting this causes all sorts of extra checks to occur
    // the overhead may cause slower systems to not perform as fast
    // as the rest of the network, caution is advised when using this.
    bool PARANOID_MODE;

    // SCP config
    SecretKey NODE_SEED;
    bool NODE_IS_VALIDATOR;
    stellar::SCPQuorumSet QUORUM_SET;

    std::map<std::string, std::string> VALIDATOR_NAMES;

    // History config
    std::map<std::string, std::shared_ptr<HistoryArchive>> HISTORY;

    // Database config
    std::string DATABASE;

    std::vector<std::string> COMMANDS;
    std::vector<std::string> REPORT_METRICS;

    Config();

    void load(std::string const& filename);

    std::string toShortString(PublicKey const& pk) const;
};
}
