#include "generated/stellar.hh"

namespace stellar
{
    class Config
    {
        
    public:
        int PEER_PORT;
        stellarxdr::uint256 VALIDATION_SEED;
        unsigned TARGET_PEER_CONNECTIONS;
        unsigned MAX_PEER_CONNECTIONS;

        // Peers we will always try to stay connected to
        std::vector<std::string> PREFERRED_PEERS;
        std::vector<std::string> KNOWN_PEERS;


        bool RUN_STANDALONE;
        int PROTOCOL_VERSION;
        std::string VERSION_STR;

        std::string LOG_FILE_PATH;

        Config();

        void load(std::string const& filename);
    };
}
