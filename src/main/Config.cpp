
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Config.h"
#include "history/HistoryArchive.h"
#include "generated/StellarCoreVersion.h"
#include "lib/util/cpptoml.h"
#include "util/Logging.h"
#include "util/types.h"
#include "crypto/Base58.h"

namespace stellar
{
Config::Config() : PEER_KEY(SecretKey::random())
{
    // fill in defaults

    // non configurable
    PROTOCOL_VERSION = 1;
    VERSION_STR = STELLAR_CORE_VERSION;
    REBUILD_DB = false;
    DESIRED_BASE_RESERVE = 10000000;
    FORCE_SCP = false;

    // configurable
    DESIRED_BASE_FEE = 10;
    DESIRED_MAX_TX_PER_LEDGER = 500;
    PEER_PORT = DEFAULT_PEER_PORT;
    RUN_STANDALONE = false;
    MANUAL_CLOSE = false;
    CATCHUP_COMPLETE = false;
    ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
    ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = false;
    ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = false;
    BREAK_ASIO_LOOP_FOR_FAST_TESTS = false;
    TARGET_PEER_CONNECTIONS = 20;
    MAX_PEER_CONNECTIONS = 50;
    LOG_FILE_PATH = "stellar-core.log";
    TMP_DIR_PATH = "tmp";
    BUCKET_DIR_PATH = "buckets";
    HTTP_PORT = 39132;
    PUBLIC_HTTP_PORT = false;
    PEER_PUBLIC_KEY = PEER_KEY.getPublicKey();

    DATABASE = "sqlite3://:memory:";
}

void
loadQset(std::shared_ptr<cpptoml::toml_group> group, SCPQuorumSet& qset,
         int level)
{
    assert(level <= 2);

    for (auto& item : *group)
    {
        if (item.first == "THRESHOLD")
        {
            qset.threshold = (uint32_t)item.second->as<int64_t>()->value();
        }
        else if (item.first == "VALIDATORS")
        {
            for (auto v : item.second->as_array()->array())
            {
                uint256 p = fromBase58Check256(VER_ACCOUNT_ID,
                                               v->as<std::string>()->value());
                qset.validators.push_back(p);
            }
        }
        else
        { // must be a subset
            if (level < 2)
            {
                qset.innerSets.resize((uint32_t)qset.innerSets.size() + 1);
                loadQset(item.second->as_group(),
                         qset.innerSets[qset.innerSets.size() - 1], level + 1);
            }
        }
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
                int64_t parsedPort = item.second->as<int64_t>()->value();
                if (parsedPort <= 0 || parsedPort > UINT16_MAX)
                    throw std::invalid_argument("bad port number");
                PEER_PORT = static_cast<unsigned short>(parsedPort);
            }
            else if (item.first == "HTTP_PORT")
            {
                int64_t parsedPort = item.second->as<int64_t>()->value();
                if (parsedPort <= 0 || parsedPort > UINT16_MAX)
                    throw std::invalid_argument("bad port number");
                HTTP_PORT = static_cast<unsigned short>(parsedPort);
            }
            else if (item.first == "PUBLIC_HTTP_PORT")
                PUBLIC_HTTP_PORT = item.second->as<bool>()->value();
            else if (item.first == "DESIRED_BASE_FEE")
                DESIRED_BASE_FEE =
                    (uint32_t)item.second->as<int64_t>()->value();
            else if (item.first == "DESIRED_MAX_TX_PER_LEDGER")
                DESIRED_MAX_TX_PER_LEDGER =
                    (uint32_t)item.second->as<int64_t>()->value();
            else if (item.first == "RUN_STANDALONE")
                RUN_STANDALONE = item.second->as<bool>()->value();
            else if (item.first == "CATCHUP_COMPLETE")
                CATCHUP_COMPLETE = item.second->as<bool>()->value();
            else if (item.first == "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING")
                ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING =
                    item.second->as<bool>()->value();
            else if (item.first == "ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING")
                ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING =
                    item.second->as<bool>()->value();
            else if (item.first == "MANUAL_CLOSE")
                MANUAL_CLOSE = item.second->as<bool>()->value();
            else if (item.first == "LOG_FILE_PATH")
                LOG_FILE_PATH = item.second->as<std::string>()->value();
            else if (item.first == "TMP_DIR_PATH")
                TMP_DIR_PATH = item.second->as<std::string>()->value();
            else if (item.first == "BUCKET_DIR_PATH")
                BUCKET_DIR_PATH = item.second->as<std::string>()->value();
            else if (item.first == "VALIDATION_SEED")
            {
                std::string seed = item.second->as<std::string>()->value();
                VALIDATION_KEY = SecretKey::fromBase58Seed(seed);
            }
            else if (item.first == "PEER_SEED")
            {
                std::string seed = item.second->as<std::string>()->value();
                PEER_KEY = SecretKey::fromBase58Seed(seed);
                PEER_PUBLIC_KEY = PEER_KEY.getPublicKey();
            }
            else if (item.first == "TARGET_PEER_CONNECTIONS")
                TARGET_PEER_CONNECTIONS =
                    (int)item.second->as<int64_t>()->value();
            else if (item.first == "MAX_PEER_CONNECTIONS")
                MAX_PEER_CONNECTIONS = (int)item.second->as<int64_t>()->value();
            else if (item.first == "PREFERRED_PEERS")
            {
                for (auto v : item.second->as_array()->array())
                {
                    PREFERRED_PEERS.push_back(v->as<std::string>()->value());
                }
            }
            else if (item.first == "KNOWN_PEERS")
            {
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
                for (auto v : item.second->as_array()->array())
                {
                    COMMANDS.push_back(v->as<std::string>()->value());
                }
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
            }
            else if (item.first == "DATABASE")
                DATABASE = item.second->as<std::string>()->value();
            else
            {
                std::string err("Unknown configuration entry: '");
                err += item.first;
                err += "'";
                throw std::invalid_argument(err);
            }
        }
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
}
