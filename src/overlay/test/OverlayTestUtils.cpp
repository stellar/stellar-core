// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/test/OverlayTestUtils.h"
#include "main/Application.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"

namespace stellar
{

namespace overlaytestutils
{

uint64_t
getOverlayFloodMessageCount(std::shared_ptr<Application> app,
                            std::string const& name)
{
    return app->getMetrics()
        .NewMeter({"overlay", "flood", name}, "message")
        .count();
}

uint64_t
getAdvertisedHashCount(std::shared_ptr<Application> app)
{
    return getOverlayFloodMessageCount(app, "advertised");
}

uint64_t
getFulfilledDemandCount(std::shared_ptr<Application> app)
{
    return getOverlayFloodMessageCount(app, "fulfilled");
}

uint64_t
getUnfulfilledDemandCount(std::shared_ptr<Application> app)
{
    return getOverlayFloodMessageCount(app, "unfulfilled-unknown") +
           getOverlayFloodMessageCount(app, "unfulfilled-banned");
}

uint64_t
getUnknownDemandCount(std::shared_ptr<Application> app)
{
    return getOverlayFloodMessageCount(app, "unfulfilled-unknown");
}

uint64_t
getSentDemandCount(std::shared_ptr<Application> app)
{
    return app->getOverlayManager()
        .getOverlayMetrics()
        .mSendFloodDemandMeter.count();
}

}

}
