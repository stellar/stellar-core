// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "overlay/PeerManager.h"
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace stellar
{

class Application;
class Peer;
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

bool knowsAsPreferred(Application& knowingApp, Application& knownApp);

int numberOfAppConnections(Application& app);

int numberOfSimulationConnections(std::shared_ptr<Simulation> simulation);

std::shared_ptr<Peer> getPeerConnectedTo(Application& from, Application& to);

std::pair<std::shared_ptr<Peer>, std::shared_ptr<Peer>> crankUntilAuthenticated(
    std::shared_ptr<Simulation> simulation, Application& a, Application& b,
    std::chrono::seconds timeout = std::chrono::seconds(30));

void
crankUntilDisconnected(std::shared_ptr<Simulation> simulation,
                       std::shared_ptr<Peer> const& p0,
                       std::shared_ptr<Peer> const& p1,
                       std::chrono::seconds timeout = std::chrono::seconds(30));

std::shared_ptr<StellarMessage> makeStellarMessage(uint32_t wasmSize);
}
}
