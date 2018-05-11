#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrame.h"
#include "transport/Peer.h"
#include "xdr/Stellar-overlay.h"

#include <string>

namespace stellar
{

struct ItemKey;

class MessageHandler
{
  public:
    virtual ~MessageHandler();

    virtual bool shouldAbort(Peer::const_pointer peer) = 0;
    virtual std::pair<bool, std::string> acceptHello(Peer::pointer peer,
                                                     Hello const& elo) = 0;
    virtual bool acceptAuthenticated(Peer::pointer peer) = 0;
    virtual void getSCPState(Peer::pointer peer, uint32 uint32_t) = 0;
    virtual void getPeers(Peer::pointer peer) = 0;
    virtual void peers(Peer::pointer peer,
                       std::vector<PeerAddress> const& peers) = 0;

    virtual void doesNotHave(Peer::pointer peer, ItemKey itemKey) = 0;
    virtual void getTxSet(Peer::pointer peer, Hash const& hash) = 0;
    virtual void getQuorumSet(Peer::pointer peer, Hash const& hash) = 0;
    virtual std::set<SCPEnvelope> txSet(Peer::pointer peer,
                                        TransactionSet const& txSet,
                                        bool force = false) = 0;
    virtual std::set<SCPEnvelope> quorumSet(Peer::pointer peer,
                                            SCPQuorumSet const& qSet) = 0;
};
}
