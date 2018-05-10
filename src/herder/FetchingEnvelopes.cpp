// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/FetchingEnvelopes.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "item/ItemFetcher.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "overlay/QSetCache.h"
#include "overlay/TxSetCache.h"
#include "scp/QuorumSetUtils.h"
#include "scp/SCPUtils.h"
#include "util/Logging.h"

#include <xdrpp/marshal.h>

namespace stellar
{

using xdr::operator<;

FetchingEnvelopes::FetchingEnvelopes(Application& app)
    : mApp(app), mNodesInQuorum(mApp)
{
}

FetchingEnvelopes::~FetchingEnvelopes()
{
}

bool
FetchingEnvelopes::handleEnvelope(Peer::pointer peer,
                                  SCPEnvelope const& envelope)
{
    if (isDiscarded(envelope))
    {
        return false;
    }

    auto items = itemsToFetch(envelope);
    if (items.empty())
    {
        return true;
    }
    else
    {
        startFetching(peer, envelope, items);
        return false;
    }
}

std::set<SCPEnvelope>
FetchingEnvelopes::handleQuorumSet(const SCPQuorumSet& q, bool force)
{
    auto itemKey = ItemKey{ItemType::QUORUM_SET, sha256(xdr::xdr_to_opaque(q))};
    CLOG(TRACE, "Herder") << "Add SCPQSet " << hexAbbrev(itemKey.getHash())
                          << (force ? ", forced" : "");

    if (!mApp.getItemFetcher().stopFetch(itemKey) && !force)
    {
        return std::set<SCPEnvelope>{};
    }

    if (!isQuorumSetSane(q, false))
    {
        discardEnvelopesWithItem(itemKey);
        return std::set<SCPEnvelope>{};
    }

    mNodesInQuorum.clear();
    mApp.getOverlayManager().getQSetCache().add(itemKey.getHash(), q);
    return processReadyItems(itemKey);
}

std::set<SCPEnvelope>
FetchingEnvelopes::handleTxSet(TxSetFramePtr txset, bool force)
{
    auto itemKey = ItemKey{ItemType::TX_SET, txset->getContentsHash()};
    CLOG(TRACE, "Herder") << "Add TxSet " << hexAbbrev(itemKey.getHash())
                          << (force ? ", forced" : "");

    if (!mApp.getItemFetcher().stopFetch(itemKey) && !force)
    {
        return std::set<SCPEnvelope>{};
    }

    mApp.getOverlayManager().getTxSetCache().add(itemKey.getHash(), txset);
    return processReadyItems(itemKey);
}

std::set<SCPEnvelope>
FetchingEnvelopes::processReadyItems(ItemKey itemKey)
{
    auto envelopes = mEnvelopeItemMap.remove(itemKey);
    for (auto& e : envelopes)
    {
        mEnvelopes[e.statement.slotIndex].mFetchingEnvelopes.erase(e);
    }
    return envelopes;
}

void
FetchingEnvelopes::discardEnvelopesWithItem(ItemKey itemKey)
{
    CLOG(TRACE, "Herder") << "Discarding SCP Envelopes with SCPQSet "
                          << hexAbbrev(itemKey.getHash());

    for (auto& envelope : mEnvelopeItemMap.envelopes(itemKey))
    {
        discardEnvelope(envelope);
    }
}

void
FetchingEnvelopes::startFetching(Peer::pointer peer,
                                 SCPEnvelope const& envelope,
                                 std::vector<ItemKey> const& items)
{
    mEnvelopes[envelope.statement.slotIndex].mFetchingEnvelopes.insert(
        envelope);
    for (auto const& itemKey : items)
    {
        mApp.getItemFetcher().fetch(peer, itemKey);
    }
    mEnvelopeItemMap.add(envelope, items);
}

void
FetchingEnvelopes::discardEnvelope(SCPEnvelope const& envelope)
{
    if (isDiscarded(envelope))
    {
        return;
    }

    mEnvelopes[envelope.statement.slotIndex].mFetchingEnvelopes.erase(envelope);
    mEnvelopes[envelope.statement.slotIndex].mDiscardedEnvelopes.insert(
        envelope);
    for (auto& item : mEnvelopeItemMap.remove(envelope))
    {
        mApp.getItemFetcher().stopFetch(item);
    }
}

std::vector<ItemKey>
FetchingEnvelopes::itemsToFetch(SCPEnvelope const& envelope) const
{
    auto result = std::vector<ItemKey>{};
    auto qSet = getQuorumSetHash(envelope);
    if (!mApp.getOverlayManager().getQSetCache().contains(qSet))
        result.push_back({ItemType::QUORUM_SET, qSet});

    auto txSetHashes = getTxSetHashes(envelope);
    for (auto const& txSetHash : txSetHashes)
        if (!mApp.getOverlayManager().getTxSetCache().contains(txSetHash))
            result.push_back({ItemType::TX_SET, txSetHash});

    return result;
}

bool
FetchingEnvelopes::isDiscarded(SCPEnvelope const& envelope)
{
    if (envelope.statement.slotIndex < mMinimumSlotIndex)
    {
        return true;
    }

    auto const& nodeID = envelope.statement.nodeID;
    if (!mNodesInQuorum.isNodeInQuorum(nodeID))
    {
        CLOG(DEBUG, "Herder")
            << "Dropping envelope from "
            << mApp.getConfig().toShortString(nodeID) << " (not in quorum)";
        return true;
    }

    auto& discardedSet =
        mEnvelopes[envelope.statement.slotIndex].mDiscardedEnvelopes;
    auto discarded =
        std::find(std::begin(discardedSet), std::end(discardedSet), envelope);
    return discarded != std::end(discardedSet);
}

void
FetchingEnvelopes::setMinimumSlotIndex(uint64_t slotIndex)
{
    // force recomputing the quorums
    mNodesInQuorum.clear();

    mMinimumSlotIndex = slotIndex;

    for (auto iter = mEnvelopes.begin(); iter != mEnvelopes.end();)
    {
        if (iter->first < mMinimumSlotIndex)
        {
            iter = mEnvelopes.erase(iter);
        }
        else
            break;
    }

    auto removed = mEnvelopeItemMap.removeIf(
        [this, slotIndex](SCPEnvelope const& envelope) {
            return envelope.statement.slotIndex < mMinimumSlotIndex;
        });

    for (auto& item : removed)
    {
        mApp.getItemFetcher().stopFetch(item);
    }
}

void
FetchingEnvelopes::dumpInfo(Json::Value& ret, size_t limit)
{
    auto& q = ret["queue"];
    auto it = mEnvelopes.rbegin();
    auto l = limit;
    while (it != mEnvelopes.rend() && l-- != 0)
    {
        if (!it->second.mFetchingEnvelopes.empty() ||
            !it->second.mDiscardedEnvelopes.empty())
        {
            auto& i = q[std::to_string(it->first)];
            dumpEnvelopes(mApp, i, it->second.mFetchingEnvelopes, "fetching");
            dumpEnvelopes(mApp, i, it->second.mDiscardedEnvelopes, "discarded");
        }
        it++;
    }
}
}
