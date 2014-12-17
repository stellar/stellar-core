#ifndef __PREFERREDPEERS__
#define __PREFERREDPEERS__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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

#endif
