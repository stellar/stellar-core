#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <string>

namespace stellar
{

class Application;

namespace overlaytestutils
{
uint64_t getAdvertisedHashCount(std::shared_ptr<Application> app);

uint64_t getFulfilledDemandCount(std::shared_ptr<Application> app);

uint64_t getUnfulfilledDemandCount(std::shared_ptr<Application> app);

uint64_t getUnknownDemandCount(std::shared_ptr<Application> app);

uint64_t getSentDemandCount(std::shared_ptr<Application> app);
uint64_t getOverlayFloodMessageCount(std::shared_ptr<Application> app,
                                     std::string const& name);
}
}
