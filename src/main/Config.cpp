#include "main/Config.h"
#include "lib/util/cpptoml.h"
#include "lib/util/Logging.h"

namespace stellar
{
    Config::Config()
    {
        // non configurable
        PROTOCOL_VERSION=1;
        VERSION_STR="0.0.1";

        TARGET_PEER_CONNECTIONS=20;
        MAX_PEER_CONNECTIONS = 50;

        
        // defaults
        PEER_PORT= 39133;
        RUN_STANDALONE=false;
       
        LOG_FILE_PATH="hayashi.log";
    }

    void Config::load(std::string& filename)
    {
        try {
            cpptoml::toml_group g = cpptoml::parse_file(filename);
            if(g.contains("PEER_PORT")) PEER_PORT = (int) g.get("PEER_PORT")->as<int64_t>()->value();
            if(g.contains("RUN_STANDALONE")) RUN_STANDALONE = g.get("RUN_STANDALONE")->as<bool>()->value();
            if(g.contains("LOG_FILE_PATH")) LOG_FILE_PATH = g.get("LOG_FILE_PATH")->as<std::string>()->value();

            if(g.contains("TARGET_PEER_CONNECTIONS")) TARGET_PEER_CONNECTIONS = (int)g.get("TARGET_PEER_CONNECTIONS")->as<int64_t>()->value();
            if(g.contains("MAX_PEER_CONNECTIONS")) MAX_PEER_CONNECTIONS = (int)g.get("MAX_PEER_CONNECTIONS")->as<int64_t>()->value();
            

        }catch(cpptoml::toml_parse_exception& ex)
        {
            LOG(ERROR) << "Failed to parse " << filename << ": " << ex.what();
        } 
    }
}