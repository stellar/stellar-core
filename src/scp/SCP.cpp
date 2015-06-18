// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SCP.h"

#include <algorithm>

#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "crypto/Hex.h"

namespace stellar
{

SCP::SCP(SCPDriver& driver, SecretKey const& secretKey,
         SCPQuorumSet const& qSetLocal)
    : mDriver(driver)
{
    mLocalNode = std::make_shared<LocalNode>(secretKey, qSetLocal, this);
}

SCP::EnvelopeState
SCP::receiveEnvelope(SCPEnvelope const& envelope)
{
    // If the envelope is not correctly signed, we ignore it.
    if (!verifyEnvelope(envelope))
    {
        CLOG(DEBUG, "SCP") << "SCP::receiveEnvelope invalid";
        return SCP::EnvelopeState::INVALID;
    }

    uint64 slotIndex = envelope.statement.slotIndex;
    return getSlot(slotIndex)->processEnvelope(envelope);
}

bool
SCP::abandonBallot(uint64 slotIndex)
{
    assert(!getSecretKey().isZero());
    return getSlot(slotIndex)->abandonBallot();
}

bool
SCP::nominate(uint64 slotIndex, Value const& value, Value const& previousValue,
              bool timedout)
{
    assert(!getSecretKey().isZero());
    return getSlot(slotIndex)->nominate(value, previousValue, timedout);
}

void
SCP::updateLocalQuorumSet(SCPQuorumSet const& qSet)
{
    mLocalNode->updateQuorumSet(qSet);
}

SCPQuorumSet const&
SCP::getLocalQuorumSet()
{
    return mLocalNode->getQuorumSet();
}

NodeID const&
SCP::getLocalNodeID()
{
    return mLocalNode->getNodeID();
}

void
SCP::purgeSlots(uint64 maxSlotIndex)
{
    auto it = mKnownSlots.begin();
    while (it != mKnownSlots.end())
    {
        if (it->first < maxSlotIndex)
        {
            it = mKnownSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::shared_ptr<LocalNode>
SCP::getLocalNode()
{
    return mLocalNode;
}

std::shared_ptr<Slot>
SCP::getSlot(uint64 slotIndex)
{
    auto it = mKnownSlots.find(slotIndex);
    if (it == mKnownSlots.end())
    {
        mKnownSlots[slotIndex] = std::make_shared<Slot>(slotIndex, *this);
    }
    return mKnownSlots[slotIndex];
}

void
SCP::signEnvelope(SCPEnvelope& envelope)
{
    assert(envelope.statement.nodeID == getSecretKey().getPublicKey());
    envelope.signature =
        getSecretKey().sign(xdr::xdr_to_opaque(envelope.statement));
    mDriver.envelopeSigned();
}

bool
SCP::verifyEnvelope(SCPEnvelope const& envelope)
{
    bool b = PublicKey::verifySig(envelope.statement.nodeID, envelope.signature,
                                  xdr::xdr_to_opaque(envelope.statement));
    mDriver.envelopeVerified(b);
    return b;
}

void
SCP::dumpInfo(Json::Value& ret)
{
    for (auto& item : mKnownSlots)
    {
        item.second->dumpInfo(ret);
    }
}

SecretKey const&
SCP::getSecretKey()
{
    return mLocalNode->getSecretKey();
}

size_t
SCP::getKnownSlotsCount() const
{
    return mKnownSlots.size();
}

size_t
SCP::getCumulativeStatemtCount() const
{
    size_t c = 0;
    for (auto const& s : mKnownSlots)
    {
        c += s.second->getStatementCount();
    }
    return c;
}
}
