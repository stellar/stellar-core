#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/Peer.h"
#include "xdr/Stellar-overlay.h"

#include <string>

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

    virtual ~EnvelopeHandler();

    virtual EnvelopeStatus envelope(Peer::pointer peer,
                                    SCPEnvelope const& envelope) = 0;

    virtual std::vector<SCPEnvelope> getSCPState(uint32 ledgerSeq) = 0;
};
}
