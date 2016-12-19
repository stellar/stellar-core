// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "medida/metrics_registry.h"
#include "herder/TxSetFrame.h"
#include "overlay/StellarXDR.h"
#include "overlay/Tracker.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "xdrpp/marshal.h"

namespace stellar
{

ItemFetcher::ItemFetcher(Application& app, AskPeer askPeer)
    : mApp(app)
    , mItemMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "item-fetch-map"}))
    , mAskPeer(askPeer)
{
}

void
ItemFetcher::fetch(Hash itemHash, const SCPEnvelope& envelope)
{
    CLOG(TRACE, "Overlay") << "fetch " << hexAbbrev(itemHash);
    auto entryIt = mTrackers.find(itemHash);
    if (entryIt == mTrackers.end())
    { // not being tracked
        TrackerPtr tracker = std::make_shared<Tracker>(mApp, itemHash, mAskPeer);
        mTrackers[itemHash] = tracker;
        mItemMapSize.inc();

        tracker->listen(envelope);
        tracker->tryNextPeer();
    }
    else
    {
        entryIt->second->listen(envelope);
    }
}

uint64
ItemFetcher::getLastSeenSlotIndex(Hash itemHash) const
{
    auto iter = mTrackers.find(itemHash);
    if (iter == mTrackers.end())
    {
        return 0;
    }

    return iter->second->getLastSeenSlotIndex();
}

void
ItemFetcher::stopFetchingBelow(uint64 slotIndex)
{
    // only perform this cleanup from the top of the stack as it causes
    // all sorts of evil side effects
    mApp.getClock().getIOService().post(
        [this, slotIndex]()
        {
            stopFetchingBelowInternal(slotIndex);
        });
}

void
ItemFetcher::stopFetchingBelowInternal(uint64 slotIndex)
{
    for (auto iter = mTrackers.begin(); iter != mTrackers.end();)
    {
        if (!iter->second->clearEnvelopesBelow(slotIndex))
        {
            iter = mTrackers.erase(iter);
            mItemMapSize.dec();
        }
        else
        {
            iter++;
        }
    }
}

void
ItemFetcher::doesntHave(Hash const& itemHash, Peer::pointer peer)
{
    const auto& iter = mTrackers.find(itemHash);
    if (iter != mTrackers.end())
    {
        iter->second->doesntHave(peer);
    }
}

void
ItemFetcher::recv(Hash itemHash)
{
    CLOG(TRACE, "Overlay") << "Recv " << hexAbbrev(itemHash);
    const auto& iter = mTrackers.find(itemHash);

    if (iter != mTrackers.end())
    {
        // this code can safely be called even if recvSCPEnvelope ends up
        // calling recv on the same itemHash
        auto& tracker = iter->second;

        CLOG(TRACE, "Overlay") << "Recv " << hexAbbrev(itemHash) << " : "
                               << tracker->size();

        while (!tracker->empty())
        {
            mApp.getHerder().recvSCPEnvelope(tracker->pop());
        }
        // stop the timer, stop requesting the item as we have it
        tracker->resetLastSeenSlotIndex();
        tracker->cancel();
    }
}

}
