// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "item/ItemPeerQueue.h"

namespace stellar
{

void
ItemPeerQueue::addKnowing(Peer::pointer peer)
{
    if (peer)
    {
        mPeersKnowing.insert(peer);
    }
}
void
ItemPeerQueue::removeKnowing(Peer::pointer peer)
{
    mPeersKnowing.erase(peer);
}

Peer::pointer
ItemPeerQueue::pop()
{
    if (mPeerQueue.empty())
    {
        return nullptr;
    }

    mLastPopped = mPeerQueue.back();
    mPeerQueue.pop_back();
    return mLastPopped;
}

Peer::pointer
ItemPeerQueue::getLastPopped() const
{
    return mLastPopped;
}

void
ItemPeerQueue::setPeers(std::vector<Peer::pointer> const& peerQueue)
{
    mPeerQueue = peerQueue;
    // move the peers that have the envelope to the back, to be processed first
    std::stable_partition(
        std::begin(mPeerQueue), std::end(mPeerQueue), [this](Peer::pointer x) {
            return mPeersKnowing.find(x) == mPeersKnowing.end();
        });
    mNumPeersSet++;
}

int
ItemPeerQueue::getNumPeersSet() const
{
    return mNumPeersSet;
}
}
