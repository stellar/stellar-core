// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "overlay/PreferredPeers.h"

namespace stellar
{
void
PreferredPeers::addPreferredPeers(vector<std::string> const& peerList)
{
    for (auto peerStr : peerList)
    {
        // LATER
    }
}

bool
PreferredPeers::isPeerPreferred(Peer::pointer peer)
{
    int port = peer->getRemoteListeningPort();
    std::string const& ip = peer->getIP();
    for (auto peerPair : mPeerList)
    {
        if ((peerPair.second == port) && (peerPair.first == ip))
            return true;
    }
    return false;
}
}
