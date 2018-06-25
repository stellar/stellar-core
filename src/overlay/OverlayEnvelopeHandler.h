#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/EnvelopeHandler.h"

namespace stellar
{
class OverlayEnvelopeHandler : public EnvelopeHandler
{
  public:
    explicit OverlayEnvelopeHandler(Application& app);
    ~OverlayEnvelopeHandler() = default;

    EnvelopeStatus handleEnvelope(Peer::pointer,
                                  SCPEnvelope const& envelope) override;

    void getQuorumSet(Peer::pointer peer, Hash const& hash) override;
    std::set<SCPEnvelope> handleQuorumSet(Peer::pointer peer,
                                          SCPQuorumSet const& qSet,
                                          bool force) override;

    void getTxSet(Peer::pointer peer, Hash const& hash) override;
    std::set<SCPEnvelope> handleTxSet(Peer::pointer peer,
                                      TransactionSet const& txSet,
                                      bool force) override;

    void doesNotHave(Peer::pointer peer, ItemKey itemKey) override;

  private:
    Application& mApp;

    medida::Timer& mRecvSCPPrepareTimer;
    medida::Timer& mRecvSCPConfirmTimer;
    medida::Timer& mRecvSCPNominateTimer;
    medida::Timer& mRecvSCPExternalizeTimer;
    medida::Meter& mEnvelopeReceive;
    medida::Counter& mEnvelopeDropped;

    EnvelopeStatus processEnvelope(Peer::pointer, SCPEnvelope const& envelope);
};
}
