#pragma once
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "generated/StellarXDR.h"
#include <string>
#include <memory>
#include <map>
#include "crypto/SecretKey.h"

#define DEFAULT_PEER_PORT 39133

namespace stellar
{
class HistoryArchive;

class Config : public std::enable_shared_from_this<Config>
{
  public:
    typedef std::shared_ptr<Config> pointer;

    enum TestDbMode
    {
        TESTDB_DEFAULT,
        TESTDB_IN_MEMORY_SQLITE,
        TESTDB_ON_DISK_SQLITE,
#ifdef USE_POSTGRES
        TESTDB_UNIX_LOCAL_POSTGRESQL,
        TESTDB_TCP_LOCALHOST_POSTGRESQL,
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

    // A config parameter that reduces ledger close time to 1s and checkpoint
    // frequency to every 8 ledgers. Do not ever set this in production, as it
    // will make your history archives incompatible with those of anyone else.
    bool ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING;

    uint32_t PROTOCOL_VERSION;
    std::string VERSION_STR;
    std::string LOG_FILE_PATH;
    std::string TMP_DIR_PATH;
    std::string BUCKET_DIR_PATH;
    uint32_t DESIRED_BASE_FEE;     // in stroops
    uint32_t DESIRED_BASE_RESERVE; // in stroops
    unsigned short HTTP_PORT;      // what port to listen for commands
    bool PUBLIC_HTTP_PORT;         // if you accept commands from not localhost

    // overlay config
    unsigned short PEER_PORT;
    SecretKey PEER_KEY;
    PublicKey PEER_PUBLIC_KEY;
    unsigned TARGET_PEER_CONNECTIONS;
    unsigned MAX_PEER_CONNECTIONS;
    // Peers we will always try to stay connected to
    std::vector<std::string> PREFERRED_PEERS;
    std::vector<std::string> KNOWN_PEERS;

    // SCP config
    SecretKey VALIDATION_KEY;
    uint32_t QUORUM_THRESHOLD;
    std::vector<uint256> QUORUM_SET;

    // History config
    std::map<std::string, std::shared_ptr<HistoryArchive>> HISTORY;

    // Database config
    std::string DATABASE;

    std::vector<std::string> COMMANDS;
    std::vector<std::string> REPORT_METRICS;

    Config();

    void load(std::string const& filename);
    void applyCommands();
};
}
