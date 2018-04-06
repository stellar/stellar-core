#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"

#include <set>
#include <vector>

namespace stellar
{

/**
 * Contains set of knowing peers that have bigger probability of already having
 * requested item and an ordinary queue of peers that will be asked about that
 * item. Each time a new list of peers is set with setPeers it is reordered in
 * such a way that knowing peers are always first.
 */
class ItemPeerQueue
{
  public:
    /**
     * Add a peer to set of 'knowing' peers.
     */
    void addKnowing(Peer::pointer peer);

    /**
     * Remove peer from set of 'knowing' peers.
     */
    void removeKnowing(Peer::pointer peer);

    /**
     * Get and remove first peer from queue, if such peer exists. If not, null
     * will be returned.
     */
    Peer::pointer pop();

    /**
     * Return peer that was recently returned by call to pop(). Used to check if
     * quick restart of fetching is required after receiving DONT_HAVE message.
     */
    Peer::pointer getLastPopped() const;

    /**
     * Sets new list of peers as a queue. It will be reorderd so knowing peers
     * will be first one returned by pop method.
     */
    void setPeers(std::vector<Peer::pointer> const& peerQueue);

    /**
     * Get number of times setPeers was called.
     */
    int getNumPeersSet() const;

  private:
    std::set<Peer::pointer> mPeersKnowing;
    std::vector<Peer::pointer> mPeerQueue;
    int mNumPeersSet{0};
    Peer::pointer mLastPopped;
};
}
