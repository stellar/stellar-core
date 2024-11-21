// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerDoor.h"
#include "overlay/TCPPeer.h"
#include "simulation/Simulation.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"

namespace stellar
{
TEST_CASE("TCPPeer lifetime", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer s = std::make_shared<Simulation>(
        Simulation::OVER_TCP, networkID, [](int i) {
            Config cfg = getTestConfig(i);
            cfg.MAX_INBOUND_PENDING_CONNECTIONS = i % 2;
            cfg.MAX_OUTBOUND_PENDING_CONNECTIONS = i % 2;
            cfg.TARGET_PEER_CONNECTIONS = i % 2;
            cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = i % 2;
            cfg.BACKGROUND_OVERLAY_PROCESSING = true;
            return cfg;
        });

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);

    SCPQuorumSet n1_qset;
    n1_qset.threshold = 1;
    n1_qset.validators.push_back(v11SecretKey.getPublicKey());
    auto n1 = s->addNode(v11SecretKey, n1_qset);

    SECTION("p0 connects to p1, but p1 can't accept, destroy TCPPeer on main")
    {
        s->addPendingConnection(v10SecretKey.getPublicKey(),
                                v11SecretKey.getPublicKey());
        s->startAllNodes();
        s->stopOverlayTick();
        s->crankForAtLeast(std::chrono::seconds(5), false);

        REQUIRE(n0->getMetrics()
                    .NewMeter({"overlay", "outbound", "attempt"}, "connection")
                    .count() == 1);
        REQUIRE(n1->getMetrics()
                    .NewMeter({"overlay", "inbound", "attempt"}, "connection")
                    .count() == 1);
    }
    SECTION("p1 connects to p0, but p1 can't initiate, destroy TCPPeer on main")
    {
        s->addPendingConnection(v11SecretKey.getPublicKey(),
                                v10SecretKey.getPublicKey());
        s->startAllNodes();
        s->stopOverlayTick();
        s->crankForAtLeast(std::chrono::seconds(5), false);
        REQUIRE(n1->getMetrics()
                    .NewMeter({"overlay", "outbound", "attempt"}, "connection")
                    .count() == 0);
        REQUIRE(n0->getMetrics()
                    .NewMeter({"overlay", "inbound", "attempt"}, "connection")
                    .count() == 0);
    }

    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(!p0);
    REQUIRE(!p1);
}

TEST_CASE("TCPPeer can communicate", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::ConfigGen cfgGen = [](int i) { return getTestConfig(i); };

    SECTION("with background processing")
    {
        cfgGen = [](int i) {
            Config cfg = getTestConfig(i);
            cfg.BACKGROUND_OVERLAY_PROCESSING = true;
            return cfg;
        };
    }
    SECTION("main thread only")
    {
        cfgGen = [](int i) {
            Config cfg = getTestConfig(i);
            cfg.BACKGROUND_OVERLAY_PROCESSING = false;
            return cfg;
        };
    }

    Simulation::pointer s =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID, cfgGen);

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);

    SCPQuorumSet n1_qset;
    n1_qset.threshold = 1;
    n1_qset.validators.push_back(v11SecretKey.getPublicKey());
    auto n1 = s->addNode(v11SecretKey, n1_qset);

    s->addPendingConnection(v10SecretKey.getPublicKey(),
                            v11SecretKey.getPublicKey());
    s->startAllNodes();
    s->crankForAtLeast(std::chrono::seconds(1), false);

    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(p0);
    REQUIRE(p1);
    REQUIRE(p0->isAuthenticatedForTesting());
    REQUIRE(p1->isAuthenticatedForTesting());
    s->stopOverlayTick();

    // Now drop peer, ensure ERROR containing "drop reason" is properly flushed
    auto& msgWrite = n0->getOverlayManager().getOverlayMetrics().mMessageWrite;
    auto prevMsgWrite = msgWrite.count();

    p0->sendGetTxSet(Hash());
    p0->sendErrorAndDrop(ERR_MISC, "test drop");
    s->crankForAtLeast(std::chrono::seconds(1), false);
    REQUIRE(!p0->isConnectedForTesting());
    REQUIRE(!p1->isConnectedForTesting());

    // p0 actually sent GET_TX_SET and ERROR
    REQUIRE(msgWrite.count() == prevMsgWrite + 2);
    s->stopAllNodes();
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

TEST_CASE("TCPPeer read malformed messages", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer s = std::make_shared<Simulation>(
        Simulation::OVER_TCP, networkID, [](int i) {
            Config cfg = getTestConfig(i);
            cfg.BACKGROUND_OVERLAY_PROCESSING = true;
            // Slow down the main thread to delay drops
            cfg.ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING =
                std::chrono::milliseconds(300);
            return cfg;
        });

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);
    auto n1 = s->addNode(v11SecretKey, n0_qset);
    s->addPendingConnection(v10SecretKey.getPublicKey(),
                            v11SecretKey.getPublicKey());
    s->startAllNodes();
    s->stopOverlayTick();
    s->crankForAtLeast(std::chrono::seconds(5), false);
    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(p0);
    REQUIRE(p1);
    REQUIRE(p0->isAuthenticatedForTesting());
    REQUIRE(p1->isAuthenticatedForTesting());

    auto& p0recvError =
        n0->getOverlayManager().getOverlayMetrics().mRecvErrorTimer;
    auto p0recvErrorCount = p0recvError.count();

    auto const& msgRead =
        n1->getOverlayManager().getOverlayMetrics().mMessageRead;
    auto msgReadPrev = msgRead.count();

    auto msg = makeStellarMessage(1);

    auto crankAndValidateDrop = [&](std::string const& dropReason,
                                    bool shouldSendError) {
        s->crankForAtLeast(std::chrono::seconds(10), false);
        REQUIRE(!p0->isConnectedForTesting());
        REQUIRE(!p1->isConnectedForTesting());
        REQUIRE(p1->mDropReason == dropReason);

        if (shouldSendError)
        {
            // p0 received ERROR from p1
            REQUIRE(p0recvErrorCount + 1 == p0recvError.count());
            // p1 did not read the next message in the socket after receiving a
            // malformed message
            REQUIRE(msgReadPrev + 1 == msgRead.count());
        }
    };

    SECTION("message size is over limit")
    {
        auto bigMessage = makeStellarMessage(MAX_MESSAGE_SIZE * 2);
        REQUIRE(xdr::xdr_size(*bigMessage) > MAX_MESSAGE_SIZE);

        p0->sendAuthenticatedMessageForTesting(bigMessage);
        p0->sendAuthenticatedMessageForTesting(makeStellarMessage(1000));
        crankAndValidateDrop("error during read", false);
    }
    SECTION("bad auth sequence")
    {
        n0->postOnOverlayThread(
            [p0, msg]() {
                // Send message without auth sequence
                AuthenticatedMessage amsg;
                amsg.v0().message = *msg;
                p0->sendXdrMessageForTesting(xdr::xdr_to_msg(amsg));
                // Follow by a regular message so there's something in the
                // socket
                p0->sendAuthenticatedMessageForTesting(msg);
            },
            "send");

        crankAndValidateDrop("unexpected auth sequence", true);
    }
    SECTION("corrupt xdr")
    {
        n0->postOnOverlayThread(
            [p0, msg]() {
                xdr::msg_ptr corruptMsg = xdr::message_t::alloc(0xff);
                p0->sendXdrMessageForTesting(std::move(corruptMsg));
                // Send a normal message to make sure there's something to read
                // in the socket
                p0->sendAuthenticatedMessageForTesting(msg);
            },
            "send");
        crankAndValidateDrop("received corrupt XDR", true);
    }
}
}
