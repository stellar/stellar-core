// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderReadyEnvelopeHandler.h"
#include "herder/Herder.h"
#include "main/Application.h"

namespace stellar
{

HerderReadyEnvelopeHandler::HerderReadyEnvelopeHandler(Application& app)
    : mApp{app}
{
}

void
HerderReadyEnvelopeHandler::readyEnvelopeAvailable()
{
    mApp.getHerder().processSCPQueue();
}
}
