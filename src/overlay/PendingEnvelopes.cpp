// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PendingEnvelopes.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "overlay/QSetCache.h"
#include "overlay/TxSetCache.h"
#include "scp/SCPUtils.h"
#include "util/Logging.h"

#include <xdrpp/marshal.h>

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app)
    : mApp(app), mFetchingEnvelopes(mApp), mReadyEnvelopes(mApp)
{
}

PendingEnvelopes::~PendingEnvelopes()
{
}

Herder::EnvelopeStatus
PendingEnvelopes::handleEnvelope(Peer::pointer peer,
                                 SCPEnvelope const& envelope)
{
    auto const& nodeID = envelope.statement.nodeID;
    if (envelope.statement.slotIndex < mMinimumSlotIndex)
    {
        CLOG(DEBUG, "Herder")
            << "Dropping envelope from "
            << mApp.getConfig().toShortString(nodeID) << " (too old)";
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    // did we discard this envelope?
    // do we already have this envelope?
    // do we have the qset
    // do we have the txset

    if (mFetchingEnvelopes.isDiscarded(envelope))
    {
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    touchItemCache(envelope);

    if (mReadyEnvelopes.seen(envelope))
    {
        return Herder::ENVELOPE_STATUS_PROCESSED;
    }

    if (mFetchingEnvelopes.handleEnvelope(peer, envelope))
    {
        mReadyEnvelopes.push(envelope);
        return Herder::ENVELOPE_STATUS_READY;
    }
    return Herder::ENVELOPE_STATUS_FETCHING;
}

std::set<SCPEnvelope>
PendingEnvelopes::handleQuorumSet(const SCPQuorumSet& q, bool force)
{
    return mFetchingEnvelopes.handleQuorumSet(q, force);
}

std::set<SCPEnvelope>
PendingEnvelopes::handleTxSet(TxSetFramePtr txset, bool force)
{
    return mFetchingEnvelopes.handleTxSet(txset, force);
}

void
PendingEnvelopes::touchItemCache(SCPEnvelope const& envelope)
{
    mApp.getOverlayManager().getQSetCache().touch(getQuorumSetHash(envelope));

    for (auto const& h : getTxSetHashes(envelope))
    {
        mApp.getOverlayManager().getTxSetCache().touch(h);
    }
}

void
PendingEnvelopes::setMinimumSlotIndex(uint64_t slotIndex)
{
    mMinimumSlotIndex = slotIndex;
    mFetchingEnvelopes.setMinimumSlotIndex(slotIndex);
    mReadyEnvelopes.setMinimumSlotIndex(slotIndex);
}

bool
PendingEnvelopes::pop(uint64_t slotIndex, SCPEnvelope& ret)
{
    return mReadyEnvelopes.pop(slotIndex, ret);
}

std::vector<uint64_t>
PendingEnvelopes::readySlots()
{
    return mReadyEnvelopes.readySlots();
}

Json::Value
PendingEnvelopes::getJsonInfo(size_t limit)
{
    Json::Value ret;
    mFetchingEnvelopes.dumpInfo(ret, limit);
    mReadyEnvelopes.dumpInfo(ret, limit);
    return ret;
}
}
