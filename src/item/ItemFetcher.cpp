// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "item/ItemFetcher.h"
#include "crypto/Hex.h"
#include "item/ItemKey.h"
#include "main/Application.h"
#include "transport/Transport.h"
#include "util/Logging.h"

#include <medida/metrics_registry.h>

namespace stellar
{

static std::chrono::milliseconds const MS_TO_WAIT_FOR_FETCH_REPLY{1500};
static int const MAX_WAIT_MULTIPLIER = 1000;

ItemFetcher::ItemFetcher(Application& app)
    : mApp(app)
    , mFetchTimer{mApp}
    , mItemMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "item-fetch-map"}))
    , mFetchItem(app.getMetrics().NewMeter(
          {"overlay", "item-fetcher", "fetch-item"}, "item-fetcher"))
    , mResetPeerQueue(app.getMetrics().NewMeter(
          {"overlay", "item-fetcher", "reset-peer-queue"}, "item-fetcher"))
{
}

void
ItemFetcher::fetch(Peer::pointer peer, ItemKey itemKey)
{
    CLOG(TRACE, "Overlay") << "Add " << hexAbbrev(itemKey.getHash()) << " from "
                           << getName(peer) << " to fetch queue";

    auto shouldFetchNow =
        mItemPeerQueues.find(itemKey) == std::end(mItemPeerQueues);
    // it also adds item to mItemPeerQueues if not existing before
    mItemPeerQueues[itemKey].addKnowing(peer);
    mItemMapSize.set_count(mItemPeerQueues.size());

    if (shouldFetchNow)
    {
        fetchNow(itemKey);
    }
}

void
ItemFetcher::fetchNow(ItemKey itemKey)
{
    mFetchQueue.push(mApp.getClock().now(), itemKey);
    fetchFromQueue();
}

bool
ItemFetcher::stopFetch(ItemKey itemKey)
{
    CLOG(TRACE, "Overlay") << "Stop fetch " << hexAbbrev(itemKey.getHash());

    return mItemPeerQueues.erase(itemKey) > 0;
}

bool
ItemFetcher::isFetching(ItemKey itemKey) const
{
    return mItemPeerQueues.find(itemKey) != std::end(mItemPeerQueues);
}

void
ItemFetcher::doesntHave(Peer::pointer peer, ItemKey itemKey)
{
    CLOG(TRACE, "Overlay") << getName(peer) << " does not have"
                           << hexAbbrev(itemKey.getHash());

    auto iter = mItemPeerQueues.find(itemKey);
    if (iter == std::end(mItemPeerQueues))
    {
        // we are no longer fetchin that one, ignore
        return;
    }

    auto& itemPeerQueue = iter->second;
    itemPeerQueue.removeKnowing(peer);
    if (itemPeerQueue.getLastPopped() == peer)
    {
        // restart immediately
        fetchNow(itemKey);
    }
}

/**
 * Return first item from mFetchQueue that has corresponding mItemPeerQueues -
 * which means, we still want to download it.
 */
std::map<ItemKey, ItemPeerQueue>::iterator
ItemFetcher::firstNotEmpty()
{
    while (!mFetchQueue.empty())
    {
        auto hash = mFetchQueue.top().second;
        auto it = mItemPeerQueues.find(hash);
        if (it == std::end(mItemPeerQueues))
        {
            mFetchQueue.pop();
        }
        else
        {
            return it;
        }
    }

    return std::end(mItemPeerQueues);
}

/**
 * Return next peer for given queue. If no authenticated peer is available a new
 * list of authenticated peers is created. Also returns timeout to be used for
 * next try.
 */
std::pair<Peer::pointer, std::chrono::milliseconds>
ItemFetcher::getNextPeerAndTimeout(ItemPeerQueue& itemPeerQueue)
{
    auto peer = itemPeerQueue.pop();
    while (peer && !peer->isAuthenticated())
    {
        peer = itemPeerQueue.pop();
    }

    if (peer)
    {
        return std::make_pair(peer, MS_TO_WAIT_FOR_FETCH_REPLY);
    }

    auto authenticatedPeers = mApp.getTransport().getRandomAuthenticatedPeers();
    assert(!authenticatedPeers.empty());

    itemPeerQueue.setPeers(authenticatedPeers);
    auto waitMultiplier =
        std::min(MAX_WAIT_MULTIPLIER, itemPeerQueue.getNumPeersSet());
    assert(waitMultiplier > 0);
    mResetPeerQueue.Mark();
    return std::make_pair(itemPeerQueue.pop(),
                          MS_TO_WAIT_FOR_FETCH_REPLY * waitMultiplier);
}

void
ItemFetcher::fetchFromQueue()
{
    // enusre that we will get at least one valid peer trying to start fetching
    if (mApp.getTransport().getAuthenticatedPeersCount() == 0)
    {
        return;
    }

    auto now = mApp.getClock().now();
    while (!mFetchQueue.empty())
    {
        auto it = firstNotEmpty();
        if (it == std::end(mItemPeerQueues))
        {
            // nothing to do now
            mFetchTimer.cancel();
            break;
        }

        assert(it->first == mFetchQueue.top().second);
        auto time = mFetchQueue.top().first;
        if (time > now)
        {
            // nothing to do now, lets try at 'time'
            mFetchTimer.expires_at(time);
            mFetchTimer.async_wait([this]() { this->fetchFromQueue(); },
                                   VirtualTimer::onFailureNoop);
            break;
        }

        auto item = it->first;
        auto timeout = fetchFrom(item, it->second);
        mFetchQueue.pushToLater(now + timeout);
    }
}

std::chrono::milliseconds
ItemFetcher::fetchFrom(ItemKey itemKey, ItemPeerQueue& itemPeerQueue)
{
    auto peerAndTimeout = getNextPeerAndTimeout(itemPeerQueue);
    auto peer = peerAndTimeout.first;
    auto timeout = peerAndTimeout.second;
    assert(peer); // not null, because we have at least one authenticated peer
                  // in the list
    mFetchItem.Mark();
    switch (itemKey.getType())
    {
    case ItemType::QUORUM_SET:
    {
        CLOG(TRACE, "Overlay")
            << "Fetch quorum set " << hexAbbrev(itemKey.getHash()) << " from "
            << getName(peer);
        peer->sendGetQuorumSet(itemKey.getHash());
        break;
    }
    case ItemType::TX_SET:
    {
        CLOG(TRACE, "Overlay")
            << "Fetch tx set " << hexAbbrev(itemKey.getHash()) << " from "
            << getName(peer);
        peer->sendGetTxSet(itemKey.getHash());
        break;
    }
    default:
    {
        assert(false);
    }
    }

    return timeout;
}

std::string
ItemFetcher::getName(Peer::pointer peer) const
{
    return peer ? mApp.getConfig().toShortString(peer->getPeerID()) : "(local)";
}
}
