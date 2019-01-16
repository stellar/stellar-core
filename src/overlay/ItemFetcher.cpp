// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetcher.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "overlay/StellarXDR.h"
#include "overlay/Tracker.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{

ItemFetcher::ItemFetcher(Application& app, AskPeer askPeer)
    : mApp(app), mAskPeer(askPeer)
{
}

void
ItemFetcher::fetch(Hash itemHash, const SCPEnvelope& envelope)
{
    CLOG(TRACE, "Overlay") << "fetch " << hexAbbrev(itemHash);
    auto entryIt = mTrackers.find(itemHash);
    if (entryIt == mTrackers.end())
    { // not being tracked
        TrackerPtr tracker =
            std::make_shared<Tracker>(mApp, itemHash, mAskPeer);
        mTrackers[itemHash] = tracker;

        tracker->listen(envelope);
        tracker->tryNextPeer();
    }
    else
    {
        entryIt->second->listen(envelope);
    }
}

void
ItemFetcher::stopFetch(Hash itemHash, const SCPEnvelope& envelope)
{
    CLOG(TRACE, "Overlay") << "stopFetch " << hexAbbrev(itemHash);
    const auto& iter = mTrackers.find(itemHash);
    if (iter != mTrackers.end())
    {
        auto const& tracker = iter->second;

        CLOG(TRACE, "Overlay")
            << "stopFetch " << hexAbbrev(itemHash) << " : " << tracker->size();
        tracker->discard(envelope);
        if (tracker->empty())
        {
            // stop the timer, stop requesting the item as no one is waiting for
            // it
            tracker->cancel();
        }
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

std::vector<SCPEnvelope>
ItemFetcher::fetchingFor(Hash itemHash) const
{
    auto result = std::vector<SCPEnvelope>{};
    auto iter = mTrackers.find(itemHash);
    if (iter == mTrackers.end())
    {
        return result;
    }

    auto const& waiting = iter->second->waitingEnvelopes();
    std::transform(
        std::begin(waiting), std::end(waiting), std::back_inserter(result),
        [](std::pair<Hash, SCPEnvelope> const& x) { return x.second; });
    return result;
}

void
ItemFetcher::stopFetchingBelow(uint64 slotIndex)
{
    // only perform this cleanup from the top of the stack as it causes
    // all sorts of evil side effects
    mApp.postOnMainThread(
        [this, slotIndex]() { stopFetchingBelowInternal(slotIndex); },
        "ItemFetcher: stopFetchingBelow");
}

void
ItemFetcher::stopFetchingBelowInternal(uint64 slotIndex)
{
    for (auto iter = mTrackers.begin(); iter != mTrackers.end();)
    {
        if (!iter->second->clearEnvelopesBelow(slotIndex))
        {
            iter = mTrackers.erase(iter);
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

        CLOG(TRACE, "Overlay")
            << "Recv " << hexAbbrev(itemHash) << " : " << tracker->size();

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
