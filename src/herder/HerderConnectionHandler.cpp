// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderConnectionHandler.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "transport/Peer.h"

namespace stellar
{

HerderConnectionHandler::HerderConnectionHandler(Application& app) : mApp{app}
{
}

void
HerderConnectionHandler::peerAuthenticated(Peer::pointer peer)
{
    // ask for SCP state if not synced
    peer->sendGetScpState(mApp.getLedgerManager().getLastClosedLedgerNum() + 1);
}
}
