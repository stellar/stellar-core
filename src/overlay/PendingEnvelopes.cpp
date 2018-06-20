// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PendingEnvelopes.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderUtils.h"
#include "main/Application.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "scp/QuorumSetUtils.h"
#include "util/Logging.h"

#include <xdrpp/marshal.h>

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app) : mApp(app)
{
}

PendingEnvelopes::~PendingEnvelopes()
{
}

EnvelopeHandler::EnvelopeStatus
PendingEnvelopes::handleEnvelope(Peer::pointer peer,
                                 SCPEnvelope const& envelope)
{
    auto const& nodeID = envelope.statement.nodeID;
    if (envelope.statement.slotIndex < mMinimumSlotIndex)
    {
        CLOG(DEBUG, "Herder")
            << "Dropping envelope from "
            << mApp.getConfig().toShortString(nodeID) << " (too old)";
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    // did we discard this envelope?
    // do we already have this envelope?
    // do we have the qset
    // do we have the txset

    if (isDiscarded(envelope))
    {
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    touchItemCache(envelope);

    auto items =
        mApp.getOverlayManager().getItemFetcher().fetchFor(envelope, peer);
    if (items.empty())
    {
        return EnvelopeHandler::ENVELOPE_STATUS_READY;
    }
    else
    {
        mEnvelopes[envelope.statement.slotIndex].mFetchingEnvelopes.insert(
            envelope);
        mEnvelopeItemMap.add(envelope, items);
        return EnvelopeHandler::ENVELOPE_STATUS_FETCHING;
    }
}

std::pair<bool, std::set<SCPEnvelope>>
PendingEnvelopes::handleQuorumSet(SCPQuorumSet const& qSet, bool force)
{
    auto addResult = mApp.getOverlayManager().getItemFetcher().add(qSet, force);
    if (!addResult.first)
    {
        if (!isQuorumSetSane(qSet, false))
        {
            discardEnvelopesWithItem(addResult.second);
        }
        return std::make_pair(false, std::set<SCPEnvelope>{});
    }

    return std::make_pair(true, processReadyItems(addResult.second));
}

std::set<SCPEnvelope>
PendingEnvelopes::handleTxSet(TransactionSet const& txSet, bool force)
{
    auto addResult =
        mApp.getOverlayManager().getItemFetcher().add(txSet, force);
    if (!addResult.first)
    {
        return std::set<SCPEnvelope>{};
    }

    return processReadyItems(addResult.second);
}

std::set<SCPEnvelope>
PendingEnvelopes::processReadyItems(ItemKey itemKey)
{
    auto envelopes = mEnvelopeItemMap.remove(itemKey);
    for (auto& e : envelopes)
    {
        mEnvelopes[e.statement.slotIndex].mFetchingEnvelopes.erase(e);
    }
    return envelopes;
}

void
PendingEnvelopes::discardEnvelope(SCPEnvelope const& envelope)
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
        mApp.getOverlayManager().getItemFetcher().forget(item);
    }
}

void
PendingEnvelopes::discardEnvelopesWithItem(ItemKey itemKey)
{
    CLOG(TRACE, "Herder") << "Discarding SCP Envelopes with item "
                          << hexAbbrev(itemKey.getHash());

    for (auto& envelope : mEnvelopeItemMap.envelopes(itemKey))
    {
        discardEnvelope(envelope);
    }
}

void
PendingEnvelopes::touchItemCache(SCPEnvelope const& envelope)
{
    mApp.getOverlayManager().getItemFetcher().touch(
        ItemKey{ItemType::QUORUM_SET, getQuorumSetHash(envelope)});

    for (auto const& h : getTxSetHashes(envelope))
    {
        mApp.getOverlayManager().getItemFetcher().touch(
            ItemKey{ItemType::TX_SET, h});
    }
}

bool
PendingEnvelopes::isDiscarded(SCPEnvelope const& envelope)
{
    if (envelope.statement.slotIndex < mMinimumSlotIndex)
    {
        return true;
    }

    auto& discardedSet =
        mEnvelopes[envelope.statement.slotIndex].mDiscardedEnvelopes;
    auto discarded =
        std::find(std::begin(discardedSet), std::end(discardedSet), envelope);
    return discarded != std::end(discardedSet);
}

void
PendingEnvelopes::setMinimumSlotIndex(uint64_t slotIndex)
{
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
        mApp.getOverlayManager().getItemFetcher().forget(item);
    }
}

Json::Value
PendingEnvelopes::getJsonInfo(size_t limit)
{
    Json::Value ret;

    auto& q = ret["queue"];
    auto it = mEnvelopes.rbegin();
    auto l = limit;
    while (it != mEnvelopes.rend() && l-- != 0)
    {
        if (!it->second.mFetchingEnvelopes.empty() ||
            !it->second.mDiscardedEnvelopes.empty())
        {
            auto& i = q[std::to_string(it->first)];
            dumpEnvelopes(mApp.getHerder(), i, it->second.mFetchingEnvelopes,
                          "fetching");
            dumpEnvelopes(mApp.getHerder(), i, it->second.mDiscardedEnvelopes,
                          "discarded");
        }
        it++;
    }

    return ret;
}
}
