// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <numeric>

#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/test/OverlayTestUtils.h"
#include "simulation/Simulation.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"

#include <numeric>

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

bool
knowsAs(Application& knowingApp, Application& knownApp, PeerType peerType)
{
    auto data = knowingApp.getOverlayManager().getPeerManager().load(
        PeerBareAddress{"127.0.0.1", knownApp.getConfig().PEER_PORT});
    if (!data.second)
    {
        return false;
    }

    return data.first.mType == static_cast<int>(peerType);
}

bool
doesNotKnow(Application& knowingApp, Application& knownApp)
{
    return !knowingApp.getOverlayManager()
                .getPeerManager()
                .load(PeerBareAddress{"127.0.0.1",
                                      knownApp.getConfig().PEER_PORT})
                .second;
}

bool
knowsAsInbound(Application& knowingApp, Application& knownApp)
{
    return knowsAs(knowingApp, knownApp, PeerType::INBOUND);
}

bool
knowsAsOutbound(Application& knowingApp, Application& knownApp)
{
    return knowsAs(knowingApp, knownApp, PeerType::OUTBOUND);
}

bool
knowsAsPreferred(Application& knowingApp, Application& knownApp)
{
    return knowsAs(knowingApp, knownApp, PeerType::PREFERRED);
}

int
numberOfAppConnections(Application& app)
{
    return app.getOverlayManager().getAuthenticatedPeersCount();
}

int
numberOfSimulationConnections(std::shared_ptr<Simulation> simulation)
{
    auto nodes = simulation->getNodes();
    auto num = std::accumulate(std::begin(nodes), std::end(nodes), 0,
                               [&](int x, Application::pointer app) {
                                   return x + numberOfAppConnections(*app);
                               });
    return num;
}

std::shared_ptr<StellarMessage>
makeStellarMessage(uint32_t wasmSize)
{
    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);

    auto randomWasm = rust_bridge::get_random_wasm(wasmSize, 0);
    uploadHF.wasm().insert(uploadHF.wasm().begin(), randomWasm.data.data(),
                           randomWasm.data.data() + randomWasm.data.size());

    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction().type(ENVELOPE_TYPE_TX);
    msg.transaction().v1().tx.operations.push_back(uploadOp);

    return std::make_shared<StellarMessage>(msg);
}
}
}
