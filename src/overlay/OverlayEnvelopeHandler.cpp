// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayEnvelopeHandler.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "overlay/BanManager.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerAuth.h"
#include "overlay/PendingEnvelopes.h"
#include "util/Logging.h"

#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <xdrpp/marshal.h>

namespace stellar
{

OverlayEnvelopeHandler::OverlayEnvelopeHandler(Application& app)
    : mApp{app}
    , mLocalNodeID{mApp.getConfig().NODE_SEED.getPublicKey()}
    , mRecvSCPPrepareTimer{app.getMetrics().NewTimer(
          {"overlay", "recv", "scp-prepare"})}
    , mRecvSCPConfirmTimer{app.getMetrics().NewTimer(
          {"overlay", "recv", "scp-confirm"})}
    , mRecvSCPNominateTimer{app.getMetrics().NewTimer(
          {"overlay", "recv", "scp-nominate"})}
    , mRecvSCPExternalizeTimer{app.getMetrics().NewTimer(
          {"overlay", "recv", "scp-externalize"})}
    , mEnvelopeReceive(
          app.getMetrics().NewMeter({"scp", "envelope", "receive"}, "envelope"))
{
}

void
OverlayEnvelopeHandler::setValidRange(uint32_t min, uint32_t max)
{
    assert(min <= max);
    mMin = min;
    mMax = max;
}

EnvelopeHandler::EnvelopeStatus
OverlayEnvelopeHandler::handleEnvelope(Peer::pointer peer,
                                       SCPEnvelope const& envelope)
{
    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay")
            << "recvSCPMessage node: "
            << mApp.getConfig().toShortString(envelope.statement.nodeID);

    auto type = envelope.statement.pledges.type();
    auto t = (type == SCP_ST_PREPARE
                  ? mRecvSCPPrepareTimer.TimeScope()
                  : (type == SCP_ST_CONFIRM
                         ? mRecvSCPConfirmTimer.TimeScope()
                         : (type == SCP_ST_EXTERNALIZE
                                ? mRecvSCPExternalizeTimer.TimeScope()
                                : (mRecvSCPNominateTimer.TimeScope()))));

    auto status = processEnvelope(peer, envelope);
    mApp.getOverlayManager().scpEnvelopeProcessed(
        peer, envelope, status == EnvelopeHandler::ENVELOPE_STATUS_READY);
    return status;
}

EnvelopeHandler::EnvelopeStatus
OverlayEnvelopeHandler::processEnvelope(Peer::pointer peer,
                                        SCPEnvelope const& envelope)
{
    if (Logging::logDebug("Overlay"))
        CLOG(DEBUG, "Overlay")
            << "recvSCPEnvelope"
            << " from: "
            << mApp.getConfig().toShortString(envelope.statement.nodeID)
            << " s:" << envelope.statement.pledges.type()
            << " i:" << envelope.statement.slotIndex
            << " a:" << mApp.getStateHuman();

    if (envelope.statement.nodeID == mLocalNodeID)
    {
        CLOG(DEBUG, "Overlay") << "recvSCPEnvelope: skipping own message";
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    // If envelopes are out of our validity brackets, we just ignore them.
    if (envelope.statement.slotIndex > mMax ||
        envelope.statement.slotIndex < mMin)
    {
        CLOG(DEBUG, "Overlay") << "Ignoring SCPEnvelope outside of range: "
                               << envelope.statement.slotIndex << "( " << mMin
                               << "," << mMax << ")";
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    auto const& nodeID = envelope.statement.nodeID;
    if (!mApp.getHerder().isNodeInQuorum(nodeID))
    {
        CLOG(DEBUG, "Herder")
            << "Dropping envelope from "
            << mApp.getConfig().toShortString(nodeID) << " (not in quorum)";
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    auto status = mApp.getOverlayManager().getPendingEnvelopes().handleEnvelope(
        nullptr, envelope);
    if (status != EnvelopeHandler::ENVELOPE_STATUS_READY)
    {
        return status;
    }

    if (mApp.getHerder().processSCPEnvelope(envelope))
    {
        return EnvelopeHandler::ENVELOPE_STATUS_READY;
    }
    else
    {
        return EnvelopeHandler::ENVELOPE_STATUS_PROCESSED;
    }
}

void
OverlayEnvelopeHandler::getQuorumSet(Peer::pointer peer, Hash const& hash)
{
    if (auto qset = mApp.getItemFetcher().getQuorumSet(hash))
    {
        peer->sendSCPQuorumSet(qset);
    }
    else
    {
        if (Logging::logTrace("Overlay"))
            CLOG(TRACE, "Overlay") << "No quorum set: " << hexAbbrev(hash);
        peer->sendDontHave(SCP_QUORUMSET, hash);
        // do we want to ask other people for it?
    }
}

std::set<SCPEnvelope>
OverlayEnvelopeHandler::handleQuorumSet(Peer::pointer peer,
                                        SCPQuorumSet const& qSet, bool force)
{
    auto result =
        mApp.getOverlayManager().getPendingEnvelopes().handleQuorumSet(qSet,
                                                                       force);
    if (result.first)
    {
        mApp.getHerder().clearNodesInQuorumCache();
    }

    for (auto const& e : result.second)
    {
        handleEnvelope(peer, e);
    }
    return result.second;
}

void
OverlayEnvelopeHandler::getTxSet(Peer::pointer peer, Hash const& hash)
{
    if (auto txSet = mApp.getItemFetcher().getTxSet(hash))
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        peer->sendMessage(newMsg);
    }
    else
    {
        peer->sendDontHave(TX_SET, hash);
    }
}

std::set<SCPEnvelope>
OverlayEnvelopeHandler::handleTxSet(Peer::pointer peer,
                                    TransactionSet const& txSet, bool force)
{
    auto envelopes = mApp.getOverlayManager().getPendingEnvelopes().handleTxSet(
        txSet, force);
    for (auto const& e : envelopes)
    {
        handleEnvelope(peer, e);
    }
    return envelopes;
}

void
OverlayEnvelopeHandler::doesNotHave(Peer::pointer peer, ItemKey itemKey)
{
    mApp.getItemFetcher().removeKnowing(peer, itemKey);
}
}
