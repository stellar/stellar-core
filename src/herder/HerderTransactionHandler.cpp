// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderTransactionHandler.h"
#include "herder/Herder.h"
#include "main/Application.h"

namespace stellar
{

HerderTransactionHandler::HerderTransactionHandler(Application& app) : mApp{app}
{
}

TransactionHandler::TransactionStatus
HerderTransactionHandler::transaction(Peer::pointer peer,
                                      TransactionEnvelope const& envelope)
{
    auto transactionFrame = TransactionFrame::makeTransactionFromWire(
        mApp.getNetworkID(), envelope);
    return transaction(peer, transactionFrame);
}

TransactionHandler::TransactionStatus
HerderTransactionHandler::transaction(
    Peer::pointer peer, TransactionFramePtr const& transactionFrame)
{
    return mApp.getHerder().recvTransaction(peer, transactionFrame);
}
}
