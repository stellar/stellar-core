// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/targets/OverlayFuzzTarget.h"

#include "crypto/SHA.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "simulation/Simulation.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/fuzz/FuzzTargetRegistry.h"
#include "test/fuzz/FuzzUtils.h"
#include "test/test.h"
#include "xdrpp/autocheck.h"

#include <filesystem>
#include <fstream>

namespace stellar
{

namespace
{

bool
isBadOverlayFuzzerInput(StellarMessage const& m)
{
    // HELLO, AUTH and ERROR_MSG messages cause the connection between
    // the peers to drop. Since peer connections are only established
    // preceding the persistent loop, a dropped peer is not only
    // inconvenient, it also confuses the fuzzer. Consider a msg A sent
    // before a peer is dropped and after a peer is dropped. The two,
    // even though the same message, will take drastically different
    // execution paths -- the fuzzer's main metric for determinism
    // (stability) and binary coverage.
    return m.type() == AUTH || m.type() == ERROR_MSG || m.type() == HELLO;
}

} // namespace

std::string
OverlayFuzzTarget::name() const
{
    return "overlay";
}

std::string
OverlayFuzzTarget::description() const
{
    return "Fuzz overlay network message handling";
}

void
OverlayFuzzTarget::initialize()
{
    reinitializeAllGlobalStateForFuzzing(1);
    FuzzUtils::generateStoredLedgerKeys(mStoredLedgerKeys.begin(),
                                        mStoredLedgerKeys.end());
    // Initialize pool IDs - overlay fuzzing doesn't use them but
    // xdr_from_fuzzer_opaque requires initialized storage
    mStoredPoolIDs.fill(PoolID{});

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    mSimulation = std::make_shared<Simulation>(Simulation::OVER_LOOPBACK,
                                               networkID, getFuzzConfig);

    SIMULATION_CREATE_NODE(10);
    SIMULATION_CREATE_NODE(11);

    // Store node IDs explicitly since map iteration order may not match
    // the order nodes were added
    mInitiatorID = v10SecretKey.getPublicKey();
    mAcceptorID = v11SecretKey.getPublicKey();

    SCPQuorumSet qSet0;
    qSet0.threshold = 2;
    qSet0.validators.push_back(v10NodeID);
    qSet0.validators.push_back(v11NodeID);

    mSimulation->addNode(v10SecretKey, qSet0);
    mSimulation->addNode(v11SecretKey, qSet0);

    mSimulation->addPendingConnection(v10SecretKey.getPublicKey(),
                                      v11SecretKey.getPublicKey());

    mSimulation->startAllNodes();

    // crank until nodes are connected
    mSimulation->crankUntil(
        [&]() {
            auto initiatorNode = mSimulation->getNode(mInitiatorID);
            auto acceptorNode = mSimulation->getNode(mAcceptorID);
            if (!initiatorNode || !acceptorNode)
            {
                return false;
            }
            auto numberOfSimulationConnections =
                acceptorNode->getOverlayManager().getAuthenticatedPeersCount() +
                initiatorNode->getOverlayManager().getAuthenticatedPeersCount();
            return numberOfSimulationConnections == 2;
        },
        std::chrono::milliseconds{500}, false);
}

FuzzResultCode
OverlayFuzzTarget::run(uint8_t const* data, size_t size)
{
    if (data == nullptr || size == 0 || size > maxInputSize())
    {
        return FuzzResultCode::FUZZ_REJECTED;
    }

    StellarMessage msg;
    std::vector<char> bins(data, data + size);

    try
    {
        xdr::xdr_from_fuzzer_opaque(mStoredLedgerKeys, mStoredPoolIDs, bins,
                                    msg);
    }
    catch (...)
    {
        // Malformed XDR - fuzzer will learn this is uninteresting
        return FuzzResultCode::FUZZ_REJECTED;
    }

    if (isBadOverlayFuzzerInput(msg))
    {
        return FuzzResultCode::FUZZ_REJECTED;
    }

    // Use the stored node IDs (set during initialize) rather than relying
    // on map iteration order from getNodeIDs()
    auto loopbackPeerConnection =
        mSimulation->getLoopbackConnection(mInitiatorID, mAcceptorID);

    if (!loopbackPeerConnection)
    {
        // Connection not found - should not happen if initialize succeeded
        return FuzzResultCode::FUZZ_REJECTED;
    }

    auto initiator = loopbackPeerConnection->getInitiator();
    auto acceptor = loopbackPeerConnection->getAcceptor();

    mSimulation->getNode(initiator->getPeerID())
        ->getClock()
        .postAction(
            [initiator, msg]() {
                initiator->Peer::sendMessage(
                    std::make_shared<StellarMessage const>(msg));
            },
            "main", Scheduler::ActionType::NORMAL_ACTION);

    mSimulation->crankForAtMost(std::chrono::milliseconds{500}, false);

    // clear all queues and cancel all events
    initiator->clearInAndOutQueues();
    acceptor->clearInAndOutQueues();

    while (mSimulation->getNode(initiator->getPeerID())
               ->getClock()
               .cancelAllEvents())
        ;
    while (mSimulation->getNode(acceptor->getPeerID())
               ->getClock()
               .cancelAllEvents())
        ;

    return FuzzResultCode::FUZZ_SUCCESS;
}

void
OverlayFuzzTarget::shutdown()
{
    if (mSimulation)
    {
        mSimulation->stopAllNodes();
    }
    mSimulation.reset();
}

size_t
OverlayFuzzTarget::maxInputSize() const
{
    // MAX_MESSAGE_SIZE from overlay - this is quite large
    // Using a reasonable upper bound for fuzzing efficiency
    return 256 * 1024; // 256KB
}

std::vector<uint8_t>
OverlayFuzzTarget::generateSeedInput()
{
    autocheck::generator<StellarMessage> gen;
    StellarMessage m(gen(16));
    while (isBadOverlayFuzzerInput(m))
    {
        m = gen(16);
    }
    auto bins = xdr::xdr_to_fuzzer_opaque(m);
    return std::vector<uint8_t>(bins.begin(), bins.end());
}

// Register the target with the global registry
REGISTER_FUZZ_TARGET(OverlayFuzzTarget, "overlay",
                     "Fuzz overlay network message handling");

} // namespace stellar
