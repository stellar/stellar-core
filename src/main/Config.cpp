
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "history/HistoryArchive.h"
#include "StellarCoreVersion.h"
#include "lib/util/cpptoml.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{
Config::Config() : PEER_KEY(SecretKey::random())
{
    // fill in defaults

    // non configurable
    LEDGER_PROTOCOL_VERSION = 1;
    OVERLAY_PROTOCOL_VERSION = 2;
    VERSION_STR = STELLAR_CORE_VERSION;
    REBUILD_DB = false;
    DESIRED_BASE_RESERVE = 10000000;
    FORCE_SCP = false;

    // configurable
    FAILURE_SAFETY = 1;
    UNSAFE_QUORUM = false;
    DESIRED_BASE_FEE = 10;
    DESIRED_MAX_TX_PER_LEDGER = 500;
    PEER_PORT = DEFAULT_PEER_PORT;
    RUN_STANDALONE = false;
    MANUAL_CLOSE = false;
    CATCHUP_COMPLETE = false;
    ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
    ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = false;
    ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = false;
    TARGET_PEER_CONNECTIONS = 20;
    MAX_PEER_CONNECTIONS = 50;
    MAX_CONCURRENT_SUBPROCESSES = 32;
    LOG_FILE_PATH = "stellar-core.log";
    TMP_DIR_PATH = "tmp";
    BUCKET_DIR_PATH = "buckets";
    HTTP_PORT = DEFAULT_PEER_PORT + 1;
    PUBLIC_HTTP_PORT = false;
    PEER_PUBLIC_KEY = PEER_KEY.getPublicKey();
    PARANOID_MODE = false;

    DATABASE = "sqlite3://:memory:";
}

void
loadQset(std::shared_ptr<cpptoml::toml_group> group, SCPQuorumSet& qset,
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
            if (!item.second->is_array())
            {
                throw std::invalid_argument("VALIDATORS must be an array");
            }
            for (auto v : item.second->as_array()->array())
            {
                if (!v->as<std::string>())
                {
                    throw std::invalid_argument("invalid VALIDATORS");
                }
                qset.validators.emplace_back(
                    PubKeyUtils::fromStrKey(v->as<std::string>()->value()));
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

        for (auto& item : g)
        {
            LOG(DEBUG) << "Config item: " << item.first;
            if (item.first == "PEER_PORT")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument("invalid PEER_PORT");
                }
                int64_t parsedPort = item.second->as<int64_t>()->value();
                if (parsedPort <= 0 || parsedPort > UINT16_MAX)
                    throw std::invalid_argument("bad port number");
                PEER_PORT = static_cast<unsigned short>(parsedPort);
            }
            else if (item.first == "HTTP_PORT")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument("invalid HTTP_PORT");
                }
                int64_t parsedPort = item.second->as<int64_t>()->value();
                if (parsedPort <= 0 || parsedPort > UINT16_MAX)
                    throw std::invalid_argument("bad port number");
                HTTP_PORT = static_cast<unsigned short>(parsedPort);
            }
            else if (item.first == "PUBLIC_HTTP_PORT")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument("invalid PUBLIC_HTTP_PORT");
                }
                PUBLIC_HTTP_PORT = item.second->as<bool>()->value();
            }
            else if (item.first == "DESIRED_BASE_FEE")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument("invalid DESIRED_BASE_FEE");
                }
                int64_t f = item.second->as<int64_t>()->value();
                if (f < 0 || f >= UINT32_MAX)
                {
                    throw std::invalid_argument("invalid DESIRED_BASE_FEE");
                }
                DESIRED_BASE_FEE = (uint32_t)f;
            }
            else if (item.first == "DESIRED_MAX_TX_PER_LEDGER")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument(
                        "invalid DESIRED_MAX_TX_PER_LEDGER");
                }
                int64_t f = item.second->as<int64_t>()->value();
                if (f <= 0 || f >= UINT32_MAX)
                {
                    throw std::invalid_argument(
                        "invalid DESIRED_MAX_TX_PER_LEDGER");
                }
                DESIRED_MAX_TX_PER_LEDGER = (uint32_t)f;
            }
            else if (item.first == "FAILURE_SAFETY")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument("invalid FAILURE_SAFETY");
                }
                int64_t f = item.second->as<int64_t>()->value();
                if (f < 0 || f >= UINT32_MAX)
                {
                    throw std::invalid_argument("invalid FAILURE_SAFETY");
                }
                FAILURE_SAFETY = (uint32_t)f;
            }
            else if (item.first == "UNSAFE_QUORUM")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument("invalid UNSAFE_QUORUM");
                }
                UNSAFE_QUORUM = item.second->as<bool>()->value();
            }
            else if (item.first == "RUN_STANDALONE")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument("invalid RUN_STANDALONE");
                }
                RUN_STANDALONE = item.second->as<bool>()->value();
            }
            else if (item.first == "CATCHUP_COMPLETE")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument("invalid CATCHUP_COMPLETE");
                }
                CATCHUP_COMPLETE = item.second->as<bool>()->value();
            }

            else if (item.first == "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument(
                        "invalid ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING");
                }
                ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING =
                    item.second->as<bool>()->value();
            }
            else if (item.first == "ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument(
                        "invalid ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING");
                }
                ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING =
                    item.second->as<bool>()->value();
            }
            else if (item.first == "MANUAL_CLOSE")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument("invalid MANUAL_CLOSE");
                }
                MANUAL_CLOSE = item.second->as<bool>()->value();
            }
            else if (item.first == "LOG_FILE_PATH")
            {
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid LOG_FILE_PATH");
                }
                LOG_FILE_PATH = item.second->as<std::string>()->value();
            }
            else if (item.first == "TMP_DIR_PATH")
            {
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid TMP_DIR_PATH");
                }
                TMP_DIR_PATH = item.second->as<std::string>()->value();
            }
            else if (item.first == "BUCKET_DIR_PATH")
            {
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid BUCKET_DIR_PATH");
                }
                BUCKET_DIR_PATH = item.second->as<std::string>()->value();
            }
            else if (item.first == "VALIDATION_SEED")
            {
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid VALIDATION_SEED");
                }
                std::string seed = item.second->as<std::string>()->value();
                VALIDATION_KEY = SecretKey::fromStrKeySeed(seed);
            }
            else if (item.first == "PEER_SEED")
            {
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid PEER_SEED");
                }
                std::string seed = item.second->as<std::string>()->value();
                PEER_KEY = SecretKey::fromStrKeySeed(seed);
                PEER_PUBLIC_KEY = PEER_KEY.getPublicKey();
            }
            else if (item.first == "TARGET_PEER_CONNECTIONS")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument(
                        "invalid TARGET_PEER_CONNECTIONS");
                }
                TARGET_PEER_CONNECTIONS =
                    (int)item.second->as<int64_t>()->value();
            }
            else if (item.first == "MAX_PEER_CONNECTIONS")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument("invalid MAX_PEER_CONNECTIONS");
                }
                MAX_PEER_CONNECTIONS = (int)item.second->as<int64_t>()->value();
            }
            else if (item.first == "PREFERRED_PEERS")
            {
                if (!item.second->is_array())
                {
                    throw std::invalid_argument(
                        "PREFERRED_PEERS must be an array");
                }
                for (auto v : item.second->as_array()->array())
                {
                    PREFERRED_PEERS.push_back(v->as<std::string>()->value());
                }
            }
            else if (item.first == "KNOWN_PEERS")
            {
                if (!item.second->is_array())
                {
                    throw std::invalid_argument("KNOWN_PEERS must be an array");
                }
                for (auto v : item.second->as_array()->array())
                {
                    KNOWN_PEERS.push_back(v->as<std::string>()->value());
                }
            }
            else if (item.first == "QUORUM_SET")
            {
                loadQset(item.second->as_group(), QUORUM_SET, 0);
            }
            else if (item.first == "COMMANDS")
            {
                if (!item.second->is_array())
                {
                    throw std::invalid_argument("COMMANDS must be an array");
                }
                for (auto v : item.second->as_array()->array())
                {
                    COMMANDS.push_back(v->as<std::string>()->value());
                }
            }
            else if (item.first == "MAX_CONCURRENT_SUBPROCESSES")
            {
                if (!item.second->as<int64_t>())
                {
                    throw std::invalid_argument(
                        "invalid MAX_CONCURRENT_SUBPROCESSES");
                }
                MAX_CONCURRENT_SUBPROCESSES =
                    (size_t)item.second->as<int64_t>()->value();
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
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid DATABASE");
                }
                DATABASE = item.second->as<std::string>()->value();
            }
            else if (item.first == "PARANOID_MODE")
            {
                if (!item.second->as<bool>())
                {
                    throw std::invalid_argument("invalid PARANOID_MODE");
                }
                PARANOID_MODE = item.second->as<bool>()->value();
            }
            else if (item.first == "NETWORK_PASSPHRASE")
            {
                if (!item.second->as<std::string>())
                {
                    throw std::invalid_argument("invalid NETWORK_PASSPHRASE");
                }
                NETWORK_PASSPHRASE = item.second->as<std::string>()->value();
            }
            else
            {
                std::string err("Unknown configuration entry: '");
                err += item.first;
                err += "'";
                throw std::invalid_argument(err);
            }
        }
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
    if (FAILURE_SAFETY == 0 && UNSAFE_QUORUM == false)
    {
        LOG(ERROR) << "Can't have FAILURE_SAFETY=0 unless you also set "
                      "UNSAFE_QUORUM=true. Be sure you know what you are "
                      "doing!";
        throw std::invalid_argument("SCP unsafe");
    }

    unsigned int topSize = (unsigned int)(QUORUM_SET.validators.size() +
                                          QUORUM_SET.innerSets.size());

    if (topSize < 3 * FAILURE_SAFETY + 1)
    {
        LOG(ERROR) << "Not enough nodes in your Quorum set to ensure your "
                      "desired level of FAILURE_SAFETY.";
        throw std::invalid_argument("SCP unsafe");
    }

    unsigned int minSize = 1 + (topSize * 67 - 1) / 100;
    if (QUORUM_SET.threshold < minSize && UNSAFE_QUORUM == false)
    {
        LOG(ERROR) << "Your THESHOLD_PERCENTAGE is too low. If you really want "
                      "this set UNSAFE_QUORUM=true. Be sure you know what you "
                      "are doing!";
        throw std::invalid_argument("SCP unsafe");
    }
}
}
