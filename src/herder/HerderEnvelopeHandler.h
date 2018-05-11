#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/EnvelopeHandler.h"

namespace stellar
{
class HerderEnvelopeHandler : public EnvelopeHandler
{
  public:
    explicit HerderEnvelopeHandler(Application& app);
    ~HerderEnvelopeHandler() = default;

    EnvelopeStatus envelope(Peer::pointer,
                            SCPEnvelope const& envelope) override;

    std::vector<SCPEnvelope> getSCPState(uint32 ledgerSeq) override;

    EnvelopeStatus processEnvelope(Peer::pointer peer,
                                   SCPEnvelope const& envelope);

  private:
    Application& mApp;

    medida::Timer& mRecvSCPPrepareTimer;
    medida::Timer& mRecvSCPConfirmTimer;
    medida::Timer& mRecvSCPNominateTimer;
    medida::Timer& mRecvSCPExternalizeTimer;
    medida::Meter& mEnvelopeReceive;
};
}
