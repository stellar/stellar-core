#include "generated/stellar.hh"

namespace stellar
{
    class Config
    {
    public:
        int PEER_PORT;
        stellarxdr::uint256 VALIDATION_SEED;

        bool RUN_STANDALONE;
        int PROTOCOL_VERSION;
        std::string VERSION_STR;
    };
}
