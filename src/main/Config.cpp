// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Config.h"
#include "history/HistoryArchive.h"
#include "generated/StellardVersion.h"
#include "lib/util/cpptoml.h"
#include "util/Logging.h"
#include "util/types.h"
#include "crypto/Base58.h"

namespace stellar
{
Config::Config()
{
    // fill in defaults

    // non configurable
    PROTOCOL_VERSION = 1;
    VERSION_STR = STELLARD_VERSION;

    // configurable
    START_NEW_NETWORK = false;
    DESIRED_BASE_FEE = 10;
    PEER_PORT = 39133;
    RUN_STANDALONE = false;
    TARGET_PEER_CONNECTIONS = 20;
    MAX_PEER_CONNECTIONS = 50;
    LOG_FILE_PATH = "stellard.log";
    QUORUM_THRESHOLD = 1000;
    HTTP_PORT = 39132;
    PUBLIC_HTTP_PORT = false;

    DATABASE = "sqlite3://:memory:";

}

void
Config::load(const std::string& filename)
{
    try
    {
        cpptoml::toml_group g = cpptoml::parse_file(filename);
        if (g.contains("PEER_PORT"))
            PEER_PORT = (int)g.get("PEER_PORT")->as<int64_t>()->value();
        if (g.contains("HTTP_PORT"))
            HTTP_PORT = (int)g.get("HTTP_PORT")->as<int64_t>()->value();
        if (g.contains("PUBLIC_HTTP_PORT"))
            PUBLIC_HTTP_PORT = g.get("PUBLIC_HTTP_PORT")->as<bool>()->value();

        if (g.contains("QUORUM_THRESHOLD"))
            QUORUM_THRESHOLD =
                (int)g.get("QUORUM_THRESHOLD")->as<int64_t>()->value();
        if (g.contains("DESIRED_BASE_FEE"))
            DESIRED_BASE_FEE =
                (uint32_t)g.get("DESIRED_BASE_FEE")->as<int64_t>()->value();

        if (g.contains("RUN_STANDALONE"))
            RUN_STANDALONE = g.get("RUN_STANDALONE")->as<bool>()->value();
        if (g.contains("LOG_FILE_PATH"))
            LOG_FILE_PATH = g.get("LOG_FILE_PATH")->as<std::string>()->value();

       

        if (g.contains("TARGET_PEER_CONNECTIONS"))
            TARGET_PEER_CONNECTIONS =
                (int)g.get("TARGET_PEER_CONNECTIONS")->as<int64_t>()->value();
        if (g.contains("MAX_PEER_CONNECTIONS"))
            MAX_PEER_CONNECTIONS =
                (int)g.get("MAX_PEER_CONNECTIONS")->as<int64_t>()->value();
        if (g.contains("PREFERRED_PEERS"))
        {
            for (auto v : g.get_array("PREFERRED_PEERS")->array())
            {
                PREFERRED_PEERS.push_back(v->as<std::string>()->value());
            }
        }

        if (g.contains("KNOWN_PEERS"))
        {
            for (auto v : g.get_array("KNOWN_PEERS")->array())
            {
                KNOWN_PEERS.push_back(v->as<std::string>()->value());
            }
        }

        if (g.contains("QUORUM_SET"))
        {
            for (auto v : g.get_array("QUORUM_SET")->array())
            {
                uint256 p = fromBase58Check256(VER_NODE_PUBLIC,
                                               v->as<std::string>()->value());
                QUORUM_SET.push_back(p);
            }
        }

        if (g.contains("HISTORY"))
        {
            auto hist = g.get_group("HISTORY");
            if (hist)
            {
                for (auto const& archive : *hist)
                {
                    auto tab = archive.second->as_group();
                    if (!tab)
                        continue;
                    std::string get, put;
                    auto gg = tab->get_as<std::string>("get");
                    auto pp = tab->get_as<std::string>("put");
                    if (gg)
                        get = *gg;
                    if (pp)
                        put = *pp;
                    HISTORY[archive.first] =
                        std::make_shared<HistoryArchive>(
                            archive.first,
                            get, put);
                }
            }
        }

        if (g.contains("DATABASE"))
            DATABASE = g.get("DATABASE")->as<std::string>()->value();

    }
    catch (cpptoml::toml_parse_exception& ex)
    {
        LOG(ERROR) << "Failed to parse " << filename << ": " << ex.what();
    }
}
}
