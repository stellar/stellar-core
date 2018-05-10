#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/ConnectionHandler.h"

namespace stellar
{
class HerderConnectionHandler : public ConnectionHandler
{
  public:
    explicit HerderConnectionHandler(Application& app);
    ~HerderConnectionHandler() = default;

    void peerAuthenticated(Peer::pointer peer) override;

  private:
    Application& mApp;
};
}
