// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderEnvelopeHandler.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "overlay/PendingEnvelopes.h"
#include "util/Logging.h"

#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>

namespace stellar
{

using xdr::operator<;

HerderEnvelopeHandler::HerderEnvelopeHandler(Application& app)
    : mApp{app}
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

EnvelopeHandler::EnvelopeStatus
HerderEnvelopeHandler::envelope(Peer::pointer peer, SCPEnvelope const& envelope)
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
        peer, mApp.getHerder().getCurrentLedgerSeq(), envelope, status);
    return status;
}

std::vector<SCPEnvelope>
HerderEnvelopeHandler::getSCPState(uint32 ledgerSeq)
{
    return mApp.getHerder().getSCPState(ledgerSeq);
}

EnvelopeHandler::EnvelopeStatus
HerderEnvelopeHandler::processEnvelope(Peer::pointer peer,
                                       SCPEnvelope const& envelope)
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder")
            << "recvSCPEnvelope"
            << " from: "
            << mApp.getConfig().toShortString(envelope.statement.nodeID)
            << " s:" << envelope.statement.pledges.type()
            << " i:" << envelope.statement.slotIndex
            << " a:" << mApp.getStateHuman();

    if (!mApp.getHerder().isValidEnvelope(envelope))
    {
        return EnvelopeHandler::ENVELOPE_STATUS_DISCARDED;
    }

    auto status = mApp.getPendingEnvelopes().handleEnvelope(peer, envelope);
    if (status == EnvelopeHandler::ENVELOPE_STATUS_READY)
    {
        mApp.getHerder().processSCPQueue();
    }
    return status;
}
}
