// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetcher.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "overlay/Tracker.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

ItemFetcher::ItemFetcher(Application& app, AskPeer askPeer)
    : mApp(app), mAskPeer(askPeer)
{
}

void
ItemFetcher::fetch(Hash const& itemHash, SCPEnvelope const& envelope)
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "fetch {}", hexAbbrev(itemHash));
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
ItemFetcher::stopFetch(Hash const& itemHash, SCPEnvelope const& envelope)
{
    ZoneScoped;
    auto const& iter = mTrackers.find(itemHash);
    if (iter != mTrackers.end())
    {
        auto const& tracker = iter->second;

        CLOG_TRACE(Overlay, "stopFetch {} : {}", hexAbbrev(itemHash),
                   tracker->size());
        tracker->discard(envelope);
        if (tracker->empty())
        {
            // stop the timer, stop requesting the item as no one is waiting for
            // it
            tracker->cancel();
        }
    }
    else
    {
        CLOG_TRACE(Overlay, "stopFetch untracked {}", hexAbbrev(itemHash));
    }
}

uint64
ItemFetcher::getLastSeenSlotIndex(Hash const& itemHash) const
{
    auto iter = mTrackers.find(itemHash);
    if (iter == mTrackers.end())
    {
        return 0;
    }

    return iter->second->getLastSeenSlotIndex();
}

std::vector<SCPEnvelope>
ItemFetcher::fetchingFor(Hash const& itemHash) const
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

std::optional<std::chrono::milliseconds>
ItemFetcher::getWaitingTime(Hash const& itemHash) const
{
    auto iter = mTrackers.find(itemHash);
    if (iter == mTrackers.end())
    {
        return std::nullopt;
    }

    return iter->second->getDuration();
}

void
ItemFetcher::stopFetchingOutsideRange(std::optional<uint64> minSlot,
                                      std::optional<uint64> maxSlot,
                                      uint64 slotToKeep)
{
    if (mTrackers.empty())
    {
        // Nothing to do. No need to post a cleanup task to the main thread.
        return;
    }
    // only perform this cleanup from the top of the stack as it causes
    // all sorts of evil side effects
    mApp.postOnMainThread(
        [this, minSlot, maxSlot, slotToKeep]() {
            stopFetchingOutsideRangeInternal(minSlot, maxSlot, slotToKeep);
        },
        "ItemFetcher: stopFetchingOutsideRange");
}

void
ItemFetcher::stopFetchingOutsideRangeInternal(std::optional<uint64> minSlot,
                                              std::optional<uint64> maxSlot,
                                              uint64 slotToKeep)
{
    ZoneScoped;
    for (auto iter = mTrackers.begin(); iter != mTrackers.end();)
    {
        if (!iter->second->clearEnvelopesOutsideRange(minSlot, maxSlot,
                                                      slotToKeep))
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
    ZoneScoped;
    auto const& iter = mTrackers.find(itemHash);
    if (iter != mTrackers.end())
    {
        iter->second->doesntHave(peer);
    }
}

void
ItemFetcher::recv(Hash const& itemHash, medida::Timer& timer)
{
    ZoneScoped;
    auto const& iter = mTrackers.find(itemHash);

    if (iter != mTrackers.end())
    {
        // this code can safely be called even if recvSCPEnvelope ends up
        // calling recv on the same itemHash
        auto& tracker = iter->second;

        CLOG_TRACE(Overlay, "Recv {} : {}", hexAbbrev(itemHash),
                   tracker->size());

        timer.Update(tracker->getDuration());
        while (!tracker->empty())
        {
            mApp.getHerder().recvSCPEnvelope(tracker->pop());
        }
        // stop the timer, stop requesting the item as we have it
        tracker->resetLastSeenSlotIndex();
        tracker->cancel();
    }
    else
    {
        CLOG_TRACE(Overlay, "Recv untracked {}", hexAbbrev(itemHash));
    }
}

#ifdef BUILD_TESTS
std::shared_ptr<Tracker>
ItemFetcher::getTracker(Hash const& h)
{
    auto it = mTrackers.find(h);
    if (it == mTrackers.end())
    {
        return nullptr;
    }
    return it->second;
}
#endif
}
