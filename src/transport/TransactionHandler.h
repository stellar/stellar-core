#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionFrame.h"
#include "transport/Peer.h"

namespace stellar
{

struct ItemKey;

class TransactionHandler
{
  public:
    enum TransactionStatus
    {
        TX_STATUS_PENDING = 0,
        TX_STATUS_DUPLICATE,
        TX_STATUS_ERROR,
        TX_STATUS_COUNT
    };

    static const char* TX_STATUS_STRING[TX_STATUS_COUNT];

    virtual ~TransactionHandler();

    virtual TransactionStatus
    transaction(Peer::pointer peer, TransactionEnvelope const& transaction) = 0;
    virtual TransactionStatus
    transaction(Peer::pointer peer, TransactionFramePtr const& transaction) = 0;
};
}
