#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/TransactionHandler.h"

namespace stellar
{
class HerderTransactionHandler : public TransactionHandler
{
  public:
    explicit HerderTransactionHandler(Application& app);
    ~HerderTransactionHandler() = default;

    TransactionStatus
    transaction(Peer::pointer peer,
                TransactionEnvelope const& transaction) override;
    TransactionStatus
    transaction(Peer::pointer peer,
                TransactionFramePtr const& transaction) override;

  private:
    Application& mApp;
};
}
