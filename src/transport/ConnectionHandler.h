#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/Peer.h"

#include <string>

namespace stellar
{

class ConnectionHandler
{
  public:
    virtual ~ConnectionHandler();

    virtual void peerAuthenticated(Peer::pointer peer) = 0;
};
}
