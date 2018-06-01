#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"
#include "transactions/TransactionFrame.h"
#include "xdr/Stellar-overlay.h"

namespace stellar
{

struct ItemKey;

class EnvelopeHandler
{
  public:
    enum EnvelopeStatus
    {
        // for some reason this envelope was discarded - either is was invalid,
        // used unsane qset or was coming from node that is not in quorum
        ENVELOPE_STATUS_DISCARDED,
        // envelope data is currently being fetched
        ENVELOPE_STATUS_FETCHING,
        // current call to recvSCPEnvelope() was the first when the envelope
        // was fully fetched so it is ready for processing
        ENVELOPE_STATUS_READY,
        // envelope was already processed
        ENVELOPE_STATUS_PROCESSED,
    };

    virtual ~EnvelopeHandler()
    {
    }

    virtual void setValidRange(uint32_t min, uint32_t max) = 0;

    /**
     * Called when envelope is received from external peer.
     */
    virtual EnvelopeStatus envelope(Peer::pointer peer,
                                    SCPEnvelope const& envelope) = 0;

    virtual void getQuorumSet(Peer::pointer peer, Hash const& hash) = 0;

    /**
     * Called when quorum set is received from external peer. When force is
     * false quorum set will be only handled when it was requested before.
     */
    virtual std::set<SCPEnvelope> quorumSet(Peer::pointer peer,
                                            SCPQuorumSet const& qSet,
                                            bool force = false) = 0;

    virtual void getTxSet(Peer::pointer peer, Hash const& hash) = 0;

    /**
     * Called when transaction set is received from external peer. When force is
     * false transaction set will be only handled when it was requested before.
     */
    virtual std::set<SCPEnvelope> txSet(Peer::pointer peer,
                                        TransactionSet const& txSet,
                                        bool force = false) = 0;

    /**
     * Called when "donthave" message is received from external peer.
     */
    virtual void doesNotHave(Peer::pointer peer, ItemKey itemKey) = 0;
};
}
