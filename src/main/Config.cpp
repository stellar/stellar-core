#include "main/Config.h"
#include "util/StellardVersion.h"
#include "lib/util/cpptoml.h"
#include "lib/util/Logging.h"
#include "lib/util/types.h"

namespace stellar
{
Config::Config()
{
    // fill in defaults

    // non configurable
    PROTOCOL_VERSION = 1;
    VERSION_STR = STELLARD_VERSION;

    // configurable
    DESIRED_BASE_FEE = 10;
    PEER_PORT = 39133;
    RUN_STANDALONE = false;
    TARGET_PEER_CONNECTIONS = 20;
    MAX_PEER_CONNECTIONS = 50;
    LOG_FILE_PATH = "stellard.log";
    QUORUM_THRESHOLD = 1000;
    HTTP_PORT = 39132;
    PUBLIC_HTTP_PORT = false;
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
                QUORUM_SET.push_back(fromBase58(v->as<std::string>()->value()));
            }
        }
    }
    catch (cpptoml::toml_parse_exception& ex)
    {
        LOG(ERROR) << "Failed to parse " << filename << ": " << ex.what();
    }
}
}
