
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "history/HistoryArchive.h"
#include "ledger/LedgerManager.h"
#include "main/ExternalQueue.h"
#include "main/StellarCoreVersion.h"
#include "scp/LocalNode.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/types.h"

#include <functional>
#include <lib/util/format.h>
#include <sstream>

namespace stellar
{
using xdr::operator<;

const uint32 Config::CURRENT_LEDGER_PROTOCOL_VERSION = 10;

Config::Config() : NODE_SEED(SecretKey::random())
{
    // fill in defaults

    // non configurable
    FORCE_SCP = false;
    LEDGER_PROTOCOL_VERSION = CURRENT_LEDGER_PROTOCOL_VERSION;

    OVERLAY_PROTOCOL_MIN_VERSION = 5;
    OVERLAY_PROTOCOL_VERSION = 6;

    VERSION_STR = STELLAR_CORE_VERSION;

    // configurable
    RUN_STANDALONE = false;
    MANUAL_CLOSE = false;
    CATCHUP_COMPLETE = false;
    CATCHUP_RECENT = 0;
    AUTOMATIC_MAINTENANCE_PERIOD = std::chrono::seconds{14400};
    AUTOMATIC_MAINTENANCE_COUNT = 50000;
    ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
    ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = false;
    ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = 0;
    ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = false;
    ALLOW_LOCALHOST_FOR_TESTING = false;
    USE_CONFIG_FOR_GENESIS = false;
    FAILURE_SAFETY = -1;
    UNSAFE_QUORUM = false;

    LOG_FILE_PATH = "stellar-core.%datetime{%Y.%M.%d-%H:%m:%s}.log";
    BUCKET_DIR_PATH = "buckets";

    TESTING_UPGRADE_DESIRED_FEE = LedgerManager::GENESIS_LEDGER_BASE_FEE;
    TESTING_UPGRADE_RESERVE = LedgerManager::GENESIS_LEDGER_BASE_RESERVE;
    TESTING_UPGRADE_MAX_TX_PER_LEDGER = 50;

    HTTP_PORT = DEFAULT_PEER_PORT + 1;
    PUBLIC_HTTP_PORT = false;
    HTTP_MAX_CLIENT = 128;
    PEER_PORT = DEFAULT_PEER_PORT;
    TARGET_PEER_CONNECTIONS = 8;
    MAX_ADDITIONAL_PEER_CONNECTIONS = -1;
    MAX_PEER_CONNECTIONS = 12;
    MAX_PENDING_CONNECTIONS = 500;
    PEER_AUTHENTICATION_TIMEOUT = 2;
    PEER_TIMEOUT = 30;
    PREFERRED_PEERS_ONLY = false;

    MINIMUM_IDLE_PERCENT = 0;

    MAX_CONCURRENT_SUBPROCESSES = 16;
    NODE_IS_VALIDATOR = false;

    DATABASE = SecretValue{"sqlite3://:memory:"};
    NTP_SERVER = "pool.ntp.org";
}

namespace
{

using ConfigItem = std::pair<std::string, std::shared_ptr<cpptoml::toml_base>>;

bool
readBool(ConfigItem const& item)
{
    if (!item.second->as<bool>())
    {
        throw std::invalid_argument(fmt::format("invalid {}", item.first));
    }
    return item.second->as<bool>()->value();
}

std::string
readString(ConfigItem const& item)
{
    if (!item.second->as<std::string>())
    {
        throw std::invalid_argument(fmt::format("invalid {}", item.first));
    }
    return item.second->as<std::string>()->value();
}

std::vector<std::string>
readStringArray(ConfigItem const& item)
{
    auto result = std::vector<std::string>{};
    if (!item.second->is_array())
    {
        throw std::invalid_argument(
            fmt::format("{} must be an array", item.first));
    }
    for (auto v : item.second->as_array()->array())
    {
        if (!v->as<std::string>())
        {
            throw std::invalid_argument(
                fmt::format("invalid element of {}", item.first));
        }
        result.push_back(v->as<std::string>()->value());
    }
    return result;
}

template <typename T>
T
readInt(ConfigItem const& item, T min = std::numeric_limits<T>::min(),
        T max = std::numeric_limits<T>::max())
{
    if (!item.second->as<int64_t>())
    {
        throw std::invalid_argument(fmt::format("invalid {}", item.first));
    }
    int64_t v = item.second->as<int64_t>()->value();
    if (v < min || v > max)
    {
        throw std::invalid_argument(fmt::format("bad {}", item.first));
    }
    return static_cast<T>(v);
}
}

void
Config::loadQset(std::shared_ptr<cpptoml::toml_group> group, SCPQuorumSet& qset,
                 int level)
{
    if (!group)
    {
        throw std::invalid_argument("invalid entry in quorum set definition");
    }

    if (level > 2)
    {
        throw std::invalid_argument("too many levels in quorum set");
    }

    int thresholdPercent = 67;
    qset.threshold = 0;

    for (auto& item : *group)
    {
        if (item.first == "THRESHOLD_PERCENT")
        {
            if (!item.second->as<int64_t>())
            {
                throw std::invalid_argument("invalid THRESHOLD_PERCENT");
            }
            int64_t f = item.second->as<int64_t>()->value();
            if (f <= 0 || f > 100)
            {
                throw std::invalid_argument("invalid THRESHOLD_PERCENT");
            }
            thresholdPercent = (uint32_t)f;
        }
        else if (item.first == "VALIDATORS")
        {
            auto values = readStringArray(item);
            for (auto v : values)
            {
                PublicKey nodeID;
                parseNodeID(v, nodeID);
                qset.validators.emplace_back(nodeID);
            }
        }
        else
        { // must be a subset
            try
            {
                if (!item.second->is_group())
                {
                    throw std::invalid_argument(
                        "invalid quorum set, should be a group");
                }
                qset.innerSets.resize((uint32_t)qset.innerSets.size() + 1);
                loadQset(item.second->as_group(),
                         qset.innerSets[qset.innerSets.size() - 1], level + 1);
            }
            catch (std::exception& e)
            {
                std::string s;
                s = e.what();
                s += " while parsing '" + item.first + "'";
                throw std::invalid_argument(s);
            }
        }
    }

    // round up: n*percent/100
    qset.threshold = uint32(
        1 +
        (((qset.validators.size() + qset.innerSets.size()) * thresholdPercent -
          1) /
         100));

    if (qset.threshold == 0 ||
        (qset.validators.empty() && qset.innerSets.empty()))
    {
        throw std::invalid_argument("invalid quorum set definition");
    }
}

void
Config::load(std::string const& filename)
{
    LOG(DEBUG) << "Loading config from: " << filename;
    try
    {
        cpptoml::toml_group g;
        if (filename == "-")
        {
            cpptoml::parser p(std::cin);
            g = p.parse();
        }
        else
        {
            g = cpptoml::parse_file(filename);
        }
        // cpptoml returns the items in non-deterministic order
        // so we need to process items that are potential dependencies first
        for (auto& item : g)
        {
            LOG(DEBUG) << "Config item: " << item.first;
            if (item.first == "PEER_PORT")
            {
                PEER_PORT = readInt<unsigned short>(item, 1, UINT16_MAX);
            }
            else if (item.first == "HTTP_PORT")
            {
                HTTP_PORT = readInt<unsigned short>(item, 1, UINT16_MAX);
            }
            else if (item.first == "HTTP_MAX_CLIENT")
            {
                HTTP_MAX_CLIENT = readInt<unsigned short>(item, 0, UINT16_MAX);
            }
            else if (item.first == "PUBLIC_HTTP_PORT")
            {
                PUBLIC_HTTP_PORT = readBool(item);
            }
            else if (item.first == "FAILURE_SAFETY")
            {
                FAILURE_SAFETY = readInt<int32_t>(item, -1, INT32_MAX - 1);
            }
            else if (item.first == "UNSAFE_QUORUM")
            {
                UNSAFE_QUORUM = readBool(item);
            }
            else if (item.first == "KNOWN_CURSORS")
            {
                KNOWN_CURSORS = readStringArray(item);
                for (auto const& c : KNOWN_CURSORS)
                {
                    if (!ExternalQueue::validateResourceID(c))
                    {
                        throw std::invalid_argument(
                            fmt::format("invalid cursor: \"{}\"", c));
                    }
                }
            }
            else if (item.first == "RUN_STANDALONE")
            {
                RUN_STANDALONE = readBool(item);
            }
            else if (item.first == "CATCHUP_COMPLETE")
            {
                CATCHUP_COMPLETE = readBool(item);
            }
            else if (item.first == "CATCHUP_RECENT")
            {
                CATCHUP_RECENT = readInt<uint32_t>(item, 0, UINT32_MAX - 1);
            }
            else if (item.first == "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING")
            {
                ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = readBool(item);
            }
            else if (item.first == "ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING")
            {
                ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = readBool(item);
            }
            else if (item.first == "ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING")
            {
                ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING =
                    readInt<uint32_t>(item, 0, UINT32_MAX - 1);
            }
            else if (item.first == "ALLOW_LOCALHOST_FOR_TESTING")
            {
                ALLOW_LOCALHOST_FOR_TESTING = readBool(item);
            }
            else if (item.first == "AUTOMATIC_MAINTENANCE_PERIOD")
            {
                AUTOMATIC_MAINTENANCE_PERIOD =
                    std::chrono::seconds{readInt<uint32_t>(item)};
            }
            else if (item.first == "AUTOMATIC_MAINTENANCE_COUNT")
            {
                AUTOMATIC_MAINTENANCE_COUNT = readInt<uint32_t>(item);
            }
            else if (item.first == "MANUAL_CLOSE")
            {
                MANUAL_CLOSE = readBool(item);
            }
            else if (item.first == "LOG_FILE_PATH")
            {
                LOG_FILE_PATH = readString(item);
            }
            else if (item.first == "TMP_DIR_PATH")
            {
                throw std::invalid_argument("TMP_DIR_PATH is not supported "
                                            "anymore - tmp data is now kept in "
                                            "BUCKET_DIR_PATH/tmp");
            }
            else if (item.first == "BUCKET_DIR_PATH")
            {
                BUCKET_DIR_PATH = readString(item);
            }
            else if (item.first == "NODE_NAMES")
            {
                auto names = readStringArray(item);
                for (auto v : names)
                {
                    PublicKey nodeID;
                    parseNodeID(v, nodeID);
                }
            }
            else if (item.first == "NODE_SEED")
            {
                PublicKey nodeID;
                parseNodeID(readString(item), nodeID, NODE_SEED, true);
            }
            else if (item.first == "NODE_IS_VALIDATOR")
            {
                NODE_IS_VALIDATOR = readBool(item);
            }
            else if (item.first == "TARGET_PEER_CONNECTIONS")
            {
                TARGET_PEER_CONNECTIONS = readInt<unsigned short>(item, 1);
            }
            else if (item.first == "MAX_PEER_CONNECTIONS")
            {
                MAX_PEER_CONNECTIONS = readInt<unsigned short>(item, 1);
            }
            else if (item.first == "MAX_ADDITIONAL_PEER_CONNECTIONS")
            {
                MAX_ADDITIONAL_PEER_CONNECTIONS =
                    readInt<int>(item, -1, UINT16_MAX);
            }
            else if (item.first == "MAX_PENDING_CONNECTIONS")
            {
                MAX_PENDING_CONNECTIONS =
                    readInt<unsigned short>(item, 1, UINT16_MAX);
            }
            else if (item.first == "PEER_AUTHENTICATION_TIMEOUT")
            {
                PEER_AUTHENTICATION_TIMEOUT =
                    readInt<unsigned short>(item, 1, UINT16_MAX);
            }
            else if (item.first == "PEER_TIMEOUT")
            {
                PEER_TIMEOUT = readInt<unsigned short>(item, 1, UINT16_MAX);
            }
            else if (item.first == "PREFERRED_PEERS")
            {
                PREFERRED_PEERS = readStringArray(item);
            }
            else if (item.first == "PREFERRED_PEER_KEYS")
            {
                // handled below
            }
            else if (item.first == "PREFERRED_PEERS_ONLY")
            {
                PREFERRED_PEERS_ONLY = readBool(item);
            }
            else if (item.first == "KNOWN_PEERS")
            {
                KNOWN_PEERS = readStringArray(item);
            }
            else if (item.first == "QUORUM_SET")
            {
                // processing performed after this loop
            }
            else if (item.first == "COMMANDS")
            {
                COMMANDS = readStringArray(item);
            }
            else if (item.first == "MAX_CONCURRENT_SUBPROCESSES")
            {
                MAX_CONCURRENT_SUBPROCESSES =
                    static_cast<size_t>(readInt<int>(item, 1));
            }
            else if (item.first == "MINIMUM_IDLE_PERCENT")
            {
                MINIMUM_IDLE_PERCENT = readInt<uint32_t>(item, 0, 100);
            }
            else if (item.first == "HISTORY")
            {
                auto hist = item.second->as_group();
                if (hist)
                {
                    for (auto const& archive : *hist)
                    {
                        LOG(DEBUG) << "History archive: " << archive.first;
                        auto tab = archive.second->as_group();
                        if (!tab)
                        {
                            throw std::invalid_argument(
                                "malformed HISTORY config block");
                        }
                        std::string get, put, mkdir;
                        for (auto const& c : *tab)
                        {
                            if (c.first == "get")
                            {
                                get = c.second->as<std::string>()->value();
                            }
                            else if (c.first == "put")
                            {
                                put = c.second->as<std::string>()->value();
                            }
                            else if (c.first == "mkdir")
                            {
                                mkdir = c.second->as<std::string>()->value();
                            }
                            else
                            {
                                std::string err(
                                    "Unknown HISTORY-table entry: '");
                                err += c.first;
                                err +=
                                    "', within [HISTORY." + archive.first + "]";
                                throw std::invalid_argument(err);
                            }
                        }
                        HISTORY[archive.first] =
                            std::make_shared<HistoryArchive>(archive.first, get,
                                                             put, mkdir);
                    }
                }
                else
                {
                    throw std::invalid_argument("incomplete HISTORY block");
                }
            }
            else if (item.first == "DATABASE")
            {
                DATABASE = SecretValue{readString(item)};
            }
            else if (item.first == "NETWORK_PASSPHRASE")
            {
                NETWORK_PASSPHRASE = readString(item);
            }
            else if (item.first == "NTP_SERVER")
            {
                NTP_SERVER = readString(item);
            }
            else if (item.first == "INVARIANT_CHECKS")
            {
                INVARIANT_CHECKS = readStringArray(item);
            }
            else
            {
                std::string err("Unknown configuration entry: '");
                err += item.first;
                err += "'";
                throw std::invalid_argument(err);
            }
        }
        // process elements that potentially depend on others
        if (g.contains("PREFERRED_PEER_KEYS"))
        {
            auto pkeys = g.get("PREFERRED_PEER_KEYS");
            if (pkeys)
            {
                auto values =
                    readStringArray(ConfigItem{"PREFERRED_PEER_KEYS", pkeys});
                for (auto v : values)
                {
                    PublicKey nodeID;
                    parseNodeID(v, nodeID);
                    PREFERRED_PEER_KEYS.push_back(KeyUtils::toStrKey(nodeID));
                }
            }
        }
        if (g.contains("QUORUM_SET"))
        {
            auto qset = g.get("QUORUM_SET");
            if (qset)
            {
                loadQset(qset->as_group(), QUORUM_SET, 0);
            }
        }
        if (MAX_ADDITIONAL_PEER_CONNECTIONS < 0)
        {
            MAX_ADDITIONAL_PEER_CONNECTIONS = TARGET_PEER_CONNECTIONS;
        }
        if (MAX_ADDITIONAL_PEER_CONNECTIONS >
            (UINT16_MAX - TARGET_PEER_CONNECTIONS))
        {
            throw std::invalid_argument(
                "invalid MAX_ADDITIONAL_PEER_CONNECTIONS");
        }
        MAX_PEER_CONNECTIONS = std::max(
            MAX_PEER_CONNECTIONS,
            static_cast<unsigned short>(MAX_ADDITIONAL_PEER_CONNECTIONS +
                                        TARGET_PEER_CONNECTIONS));

        // ensure that max pending connections is not above what the system
        // supports
        MAX_PENDING_CONNECTIONS = static_cast<unsigned short>(
            std::min<int>(MAX_PENDING_CONNECTIONS, fs::getMaxConnections()));

        // enforce TARGET_PEER_CONNECTIONS <= MAX_PEER_CONNECTIONS <=
        // MAX_PENDING_CONNECTIONS
        MAX_PEER_CONNECTIONS =
            std::min(MAX_PEER_CONNECTIONS, MAX_PENDING_CONNECTIONS);
        TARGET_PEER_CONNECTIONS =
            std::min(TARGET_PEER_CONNECTIONS, MAX_PEER_CONNECTIONS);
        validateConfig();
    }
    catch (cpptoml::toml_parse_exception& ex)
    {
        std::string err("Failed to parse '");
        err += filename;
        err += "' :";
        err += ex.what();
        throw std::invalid_argument(err);
    }
}

void
Config::validateConfig()
{
    std::set<NodeID> nodes;
    LocalNode::forAllNodes(QUORUM_SET,
                           [&](NodeID const& n) { nodes.insert(n); });

    if (nodes.size() == 0)
    {
        throw std::invalid_argument("QUORUM_SET not configured");
    }

    // calculates nodes that would break quorum
    auto selfID = NODE_SEED.getPublicKey();
    auto r = LocalNode::findClosestVBlocking(QUORUM_SET, nodes, nullptr);

    if (FAILURE_SAFETY == -1)
    {
        // calculates default value for safety giving the top level entities
        // the same weight
        // n = 3f+1 <=> f = (n-1)/3
        auto topLevelCount =
            QUORUM_SET.validators.size() + QUORUM_SET.innerSets.size();
        FAILURE_SAFETY = (static_cast<uint32>(topLevelCount) - 1) / 3;
    }

    try
    {
        if (FAILURE_SAFETY >= static_cast<int32_t>(r.size()))
        {
            LOG(ERROR) << "Not enough nodes / thresholds too strict in your "
                          "Quorum set to ensure your desired level of "
                          "FAILURE_SAFETY. Reduce FAILURE_SAFETY or fix "
                          "quorum set";
            throw std::invalid_argument(
                "FAILURE_SAFETY incompatible with QUORUM_SET");
        }

        if (!UNSAFE_QUORUM)
        {
            if (FAILURE_SAFETY == 0)
            {
                LOG(ERROR)
                    << "Can't have FAILURE_SAFETY=0 unless you also set "
                       "UNSAFE_QUORUM=true. Be sure you know what you are "
                       "doing!";
                throw std::invalid_argument("SCP unsafe");
            }

            unsigned int topSize = (unsigned int)(QUORUM_SET.validators.size() +
                                                  QUORUM_SET.innerSets.size());
            unsigned int minSize = 1 + (topSize * 2 - 1) / 3;
            if (QUORUM_SET.threshold < minSize)
            {
                LOG(ERROR)
                    << "Your THESHOLD_PERCENTAGE is too low. If you really "
                       "want "
                       "this set UNSAFE_QUORUM=true. Be sure you know what you "
                       "are doing!";
                throw std::invalid_argument("SCP unsafe");
            }
        }
    }
    catch (...)
    {
        LOG(INFO) << " Current QUORUM_SET breaks with " << r.size()
                  << " failures";
        throw;
    }
}

void
Config::parseNodeID(std::string configStr, PublicKey& retKey)
{
    SecretKey k;
    parseNodeID(configStr, retKey, k, false);
}

void
Config::parseNodeID(std::string configStr, PublicKey& retKey, SecretKey& sKey,
                    bool isSeed)
{
    if (configStr.size() < 2)
    {
        throw std::invalid_argument("invalid key");
    }

    // check if configStr is a PublicKey or a common name
    if (configStr[0] == '$')
    {
        if (isSeed)
        {
            throw std::invalid_argument("aliases only store public keys");
        }
        if (!resolveNodeID(configStr, retKey))
        {
            std::stringstream msg;
            msg << "unknown key in config: " << configStr;
            throw std::invalid_argument(msg.str());
        }
    }
    else
    {
        std::istringstream iss(configStr);
        std::string nodestr;
        iss >> nodestr;
        if (isSeed)
        {
            sKey = SecretKey::fromStrKeySeed(nodestr);
            retKey = sKey.getPublicKey();
            nodestr = sKey.getStrKeyPublic();
        }
        else
        {
            retKey = KeyUtils::fromStrKey<PublicKey>(nodestr);
        }

        if (iss)
        { // get any common name they have added
            std::string commonName;
            iss >> commonName;
            if (commonName.size())
            {
                std::string cName = "$";
                cName += commonName;
                if (resolveNodeID(cName, retKey))
                {
                    throw std::invalid_argument("name already used");
                }

                if (!VALIDATOR_NAMES
                         .emplace(std::make_pair(nodestr, commonName))
                         .second)
                {
                    std::stringstream msg;
                    msg << "naming node twice: " << commonName;
                    throw std::invalid_argument(msg.str());
                }
            }
        }
    }
}

std::string
Config::toShortString(PublicKey const& pk) const
{
    std::string ret = KeyUtils::toStrKey(pk);
    auto it = VALIDATOR_NAMES.find(ret);
    if (it == VALIDATOR_NAMES.end())
        return ret.substr(0, 5);
    else
        return it->second;
}

std::string
Config::toStrKey(PublicKey const& pk, bool& isAlias) const
{
    std::string ret = KeyUtils::toStrKey(pk);
    auto it = VALIDATOR_NAMES.find(ret);
    if (it == VALIDATOR_NAMES.end())
    {
        isAlias = false;
        return ret;
    }
    else
    {
        isAlias = true;
        return it->second;
    }
}

std::string
Config::toStrKey(PublicKey const& pk) const
{
    bool isAlias;
    return toStrKey(pk, isAlias);
}

bool
Config::resolveNodeID(std::string const& s, PublicKey& retKey) const
{
    auto expanded = expandNodeID(s);
    if (expanded.empty())
    {
        return false;
    }

    try
    {
        retKey = KeyUtils::fromStrKey<PublicKey>(expanded);
    }
    catch (std::invalid_argument&)
    {
        return false;
    }
    return true;
}

std::string
Config::expandNodeID(const std::string& s) const
{
    if (s.length() < 2)
    {
        return s;
    }
    if (s[0] != '$' && s[0] != '@')
    {
        return s;
    }

    using validatorMatcher_t =
        std::function<bool(std::pair<std::string, std::string> const&)>;
    auto arg = s.substr(1);
    auto validatorMatcher =
        s[0] == '$'
            ? validatorMatcher_t{[&](std::pair<std::string, std::string> const&
                                         p) { return p.second == arg; }}
            : validatorMatcher_t{
                  [&](std::pair<std::string, std::string> const& p) {
                      return p.first.compare(0, arg.size(), arg) == 0;
                  }};

    auto it = std::find_if(VALIDATOR_NAMES.begin(), VALIDATOR_NAMES.end(),
                           validatorMatcher);
    if (it != VALIDATOR_NAMES.end())
    {
        return it->first;
    }
    else
    {
        return {};
    }
}
}
