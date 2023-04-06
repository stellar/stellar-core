#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/PeerManager.h"
#include <memory>
#include <string>

namespace stellar
{

class Application;
class Simulation;

namespace overlaytestutils
{
uint64_t getAdvertisedHashCount(std::shared_ptr<Application> app);

uint64_t getFulfilledDemandCount(std::shared_ptr<Application> app);

uint64_t getUnfulfilledDemandCount(std::shared_ptr<Application> app);

uint64_t getUnknownDemandCount(std::shared_ptr<Application> app);

uint64_t getSentDemandCount(std::shared_ptr<Application> app);

uint64_t getOverlayFloodMessageCount(std::shared_ptr<Application> app,
                                     std::string const& name);

bool knowsAs(Application& knowingApp, Application& knownApp, PeerType peerType);

bool doesNotKnow(Application& knowingApp, Application& knownApp);

bool knowsAsInbound(Application& knowingApp, Application& knownApp);

bool knowsAsOutbound(Application& knowingApp, Application& knownApp);

int numberOfAppConnections(Application& app);

int numberOfSimulationConnections(std::shared_ptr<Simulation> simulation);

}
}
