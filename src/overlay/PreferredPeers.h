#include "Peer.h"

namespace stellar
{
    class PreferredPeers
    {
        vector< pair<std::string, int>> mPeerList;
    public:
        void addPreferredPeers(vector<std::string> const& peerList);

        bool isPeerPreferred(Peer::pointer peer);
    };
}
