// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/Peer.h"
#include "overlay/PeerManager.h"
#include "overlay/TCPPeer.h"
#include "overlay/test/LoopbackPeer.h"
#include "overlay/test/OverlayTestUtils.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/Timer.h"

#include "herder/HerderImpl.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "transactions/SignatureUtils.h"
#include <fmt/format.h>
#include <numeric>

using namespace stellar;
using namespace stellar::overlaytestutils;

namespace
{

ClaimPredicate
recursivePredicate(uint32_t counter)
{
    if (counter == 10)
    {
        ClaimPredicate u;
        u.type(CLAIM_PREDICATE_UNCONDITIONAL);
        return u;
    }

    ClaimPredicate andPred;
    andPred.type(CLAIM_PREDICATE_AND);
    andPred.andPredicates().emplace_back(recursivePredicate(counter + 1));
    andPred.andPredicates().emplace_back(recursivePredicate(counter + 1));

    return andPred;
}

Operation
getOperationGreaterThanMinMaxSizeBytes()
{
    Claimant c;
    c.v0().destination = txtest::getAccount("acc").getPublicKey();
    c.v0().predicate = recursivePredicate(0);
    CreateClaimableBalanceOp cbOp;
    cbOp.claimants.emplace_back(c);

    Operation op;
    op.body.type(CREATE_CLAIMABLE_BALANCE);
    op.body.createClaimableBalanceOp() = cbOp;

    uint32 opSize = static_cast<uint32>(xdr::xdr_argpack_size(op));
    REQUIRE(opSize > MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES);

    return op;
}

TEST_CASE("loopback peer hello", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

    REQUIRE(knowsAsOutbound(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("loopback peer with 0 port", "[overlay][connections]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto cfg2 = getTestConfig(1);
    cfg2.PEER_PORT = 0;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(!conn.getAcceptor()->isAuthenticatedForTesting());

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("loopback peer send auth before hello", "[overlay][connections]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->sendAuth();
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(!conn.getAcceptor()->isAuthenticatedForTesting());

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(doesNotKnow(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("flow control byte capacity", "[overlay][flowcontrol]")
{
    VirtualClock clock;

    auto cfg1 = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    auto cfg2 = getTestConfig(1, Config::TESTDB_IN_MEMORY);
    REQUIRE(cfg1.PEER_FLOOD_READING_CAPACITY !=
            cfg1.PEER_FLOOD_READING_CAPACITY_BYTES);

    StellarMessage tx1;
    tx1.type(TRANSACTION);
    tx1.transaction().type(ENVELOPE_TYPE_TX);

    // Make tx1 larger than the minimum we can set TX_MAX_SIZE_BYTES to.
    tx1.transaction().v1().tx.operations.emplace_back(
        getOperationGreaterThanMinMaxSizeBytes());
    auto getTxSize = [&](StellarMessage const& msg) {
        return static_cast<uint32_t>(FlowControlCapacity::msgBodySize(msg));
    };

    auto txSize = getTxSize(tx1);
    auto setupApp = [txSize, tx1](Application& app) {
        app.getHerder().setMaxClassicTxSize(txSize);

        if (appProtocolVersionStartsFrom(app, SOROBAN_PROTOCOL_VERSION))
        {
            overrideSorobanNetworkConfigForTest(app);
            modifySorobanNetworkConfig(app, [tx1](SorobanNetworkConfig& cfg) {
                cfg.mTxMaxSizeBytes =
                    static_cast<uint32_t>(xdr::xdr_size(tx1.transaction()));
            });
        }

        app.start();
    };

    auto test = [&](bool shouldRequestMore) {
        auto app1 = createTestApplication(clock, cfg1, true, false);
        auto app2 = createTestApplication(clock, cfg2, true, false);
        REQUIRE(txSize > MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES);

        setupApp(*app1);
        setupApp(*app2);

        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);
        REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());
        REQUIRE(conn.getInitiator()->checkCapacity(conn.getAcceptor()));
        REQUIRE(conn.getAcceptor()->checkCapacity(conn.getInitiator()));

        uint64_t expectedCapacity{0};
        expectedCapacity =
            app2->getOverlayManager().getFlowControlBytesTotal() - txSize;

        SECTION("basic capacity accounting")
        {
            conn.getInitiator()->getFlowControl()->setOutboundQueueLimit(
                txSize * 2);
            // Basic capacity math
            conn.getInitiator()->sendMessage(
                std::make_shared<StellarMessage>(tx1));
            REQUIRE(conn.getInitiator()
                        ->getFlowControl()
                        ->getCapacityBytes()
                        .getOutboundCapacity() == expectedCapacity);
            REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
            auto msgTracker = std::make_shared<CapacityTrackedMessage>(
                conn.getAcceptor(), tx1);
            conn.getAcceptor()->recvMessage(msgTracker);
            REQUIRE(conn.getAcceptor()
                        ->getFlowControl()
                        ->getCapacityBytes()
                        .getCapacity()
                        .mFloodCapacity == expectedCapacity);
        }
        SECTION("send more flow")
        {
            // Processing triggers SEND_MORE
            conn.getInitiator()->sendMessage(
                std::make_shared<StellarMessage>(tx1));

            auto& sendMoreMeter = app2->getOverlayManager()
                                      .getOverlayMetrics()
                                      .mSendSendMoreMeter;
            auto& sendMoreRecvMeter = app1->getOverlayManager()
                                          .getOverlayMetrics()
                                          .mRecvSendMoreTimer;
            auto currentSendCount = sendMoreMeter.count();
            auto currentRecvCount = sendMoreRecvMeter.count();
            testutil::crankSome(clock);
            REQUIRE(sendMoreMeter.count() ==
                    (currentSendCount + shouldRequestMore));
            REQUIRE(sendMoreRecvMeter.count() ==
                    (currentRecvCount + shouldRequestMore));

            // Nodes are back to full capacity
            REQUIRE(conn.getAcceptor()
                        ->getFlowControl()
                        ->getCapacityBytes()
                        .getCapacity()
                        .mFloodCapacity ==
                    app2->getOverlayManager().getFlowControlBytesTotal());
            if (shouldRequestMore)
            {
                REQUIRE(conn.getInitiator()->checkCapacity(conn.getAcceptor()));
            }
            else
            {
                REQUIRE(conn.getInitiator()
                            ->getFlowControl()
                            ->getCapacityBytes()
                            .getOutboundCapacity() ==
                        (app2->getOverlayManager().getFlowControlBytesTotal() -
                         txSize));
            }
            REQUIRE(conn.getAcceptor()->checkCapacity(conn.getInitiator()));
        }
    };

    SECTION("batch size is less than message size")
    {
        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES =
            2 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = getTxSize(tx1) / 2;
        test(true);
    }
    SECTION("batch size is greater than message size")
    {
        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES =
            2 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = 2 * getTxSize(tx1);
        // Invalid config, core will throw on startup
        REQUIRE_THROWS_AS(test(false), std::runtime_error);
    }
    SECTION("message count kicks in first")
    {
        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES =
            3 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = 2 * getTxSize(tx1);
        cfg2.PEER_FLOOD_READING_CAPACITY = 1;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE = 1;
        test(true);
    }
    SECTION("automatic calculation of byte configs")
    {
        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES = 0;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = 0;
        test(false);
    }
    SECTION("mixed versions")
    {
        cfg1.OVERLAY_PROTOCOL_VERSION = cfg1.OVERLAY_PROTOCOL_MIN_VERSION;

        cfg1.PEER_FLOOD_READING_CAPACITY_BYTES =
            2 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg1.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = getTxSize(tx1);

        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES =
            2 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = getTxSize(tx1);
        test(true);
    }
    SECTION("older versions")
    {
        cfg1.OVERLAY_PROTOCOL_VERSION = cfg1.OVERLAY_PROTOCOL_MIN_VERSION;
        cfg2.OVERLAY_PROTOCOL_VERSION = cfg2.OVERLAY_PROTOCOL_MIN_VERSION;
        cfg1.PEER_FLOOD_READING_CAPACITY_BYTES =
            2 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg1.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = getTxSize(tx1);
        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES =
            2 * getTxSize(tx1) + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = getTxSize(tx1);
        test(true);
    }
    SECTION("transaction size upgrades")
    {
        auto tx2 = tx1;
        for (uint32_t i = 0; i < tx2.transaction().v1().signatures.max_size();
             i++)
        {
            tx2.transaction().v1().signatures.emplace_back(
                SignatureUtils::sign(SecretKey::pseudoRandomForTesting(),
                                     HashUtils::pseudoRandomForTesting()));
        }
        auto txSize2 = getTxSize(tx2);
        REQUIRE(xdr::xdr_size(tx2.transaction()) >
                MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES);
        REQUIRE(txSize2 > txSize + 1);

        // Just enough buffer to fit fee-bumps, but not tx2
        auto bufferSize = 100;

        // Configure flow control such that tx2 can't be sent
        cfg1.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = txSize + 1;
        cfg1.PEER_FLOOD_READING_CAPACITY_BYTES =
            cfg1.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES + txSize + bufferSize;
        cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = txSize + 1;
        cfg2.PEER_FLOOD_READING_CAPACITY_BYTES =
            cfg2.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES + txSize + bufferSize;

        auto app1 = createTestApplication(clock, cfg1, true, false);
        auto app2 = createTestApplication(clock, cfg2, true, false);
        app1->getHerder().setFlowControlExtraBufferSize(bufferSize);
        app2->getHerder().setFlowControlExtraBufferSize(bufferSize);
        setupApp(*app1);
        setupApp(*app2);

        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);
        REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());
        REQUIRE(conn.getInitiator()->checkCapacity(conn.getAcceptor()));
        REQUIRE(conn.getAcceptor()->checkCapacity(conn.getInitiator()));

        auto upgradeApp = [&](Application::pointer app, uint32 maxTxSize) {
            ConfigUpgradeSetFrameConstPtr res;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ConfigUpgradeSet configUpgradeSet;
                auto& configEntry =
                    configUpgradeSet.updatedEntry.emplace_back();
                configEntry.configSettingID(
                    CONFIG_SETTING_CONTRACT_BANDWIDTH_V0);
                configEntry.contractBandwidth().feeTxSize1KB =
                    InitialSorobanNetworkConfig::FEE_TRANSACTION_SIZE_1KB;
                configEntry.contractBandwidth().txMaxSizeBytes = maxTxSize;
                configEntry.contractBandwidth().ledgerMaxTxsSizeBytes =
                    maxTxSize * 10;
                res = txtest::makeConfigUpgradeSet(ltx, configUpgradeSet);
                ltx.commit();
            }
            txtest::executeUpgrade(*app, txtest::makeConfigUpgrade(*res));
        };

        auto& txsRecv =
            app2->getMetrics().NewTimer({"overlay", "recv", "transaction"});
        auto start = txsRecv.count();
        conn.getInitiator()->sendMessage(std::make_shared<StellarMessage>(tx1));

        auto makeFeeBump = [&](StellarMessage const& tx) {
            StellarMessage feeBump;
            feeBump.type(TRANSACTION);
            feeBump.transaction().type(ENVELOPE_TYPE_TX_FEE_BUMP);
            feeBump.transaction().feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
            feeBump.transaction().feeBump().tx.innerTx.v1().tx =
                tx.transaction().v1().tx;
            return feeBump;
        };

        SECTION("overlay buffer is big enough")
        {
            auto feeBump = makeFeeBump(tx2);
            for (uint32_t i = 0;
                 i < feeBump.transaction().feeBump().signatures.max_size(); i++)
            {
                feeBump.transaction().feeBump().signatures.emplace_back(
                    SignatureUtils::sign(SecretKey::pseudoRandomForTesting(),
                                         HashUtils::pseudoRandomForTesting()));
            }
            REQUIRE(xdr::xdr_size(feeBump) <
                    xdr::xdr_size(tx2.transaction()) +
                        Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER);
        }
        SECTION("no upgrade, drop messages over limit")
        {
            conn.getInitiator()->sendMessage(
                std::make_shared<StellarMessage>(tx2));
            testutil::crankSome(clock);
            // First message got sent, second message got dropped (byte size is
            // over limit)
            REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
            REQUIRE(txsRecv.count() == start + 1);
        }
        SECTION("fee bump is within limit")
        {
            StellarMessage feeBump = makeFeeBump(tx1);
            // Can still send the fee-bump message, even though it's technically
            // greater than Soroban tx size limit
            conn.getInitiator()->sendMessage(
                std::make_shared<StellarMessage>(feeBump));
            testutil::crankSome(clock);
            // Both tx1 and fee-bump got sent
            REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
            REQUIRE(txsRecv.count() == start + 2);
        }
        SECTION("upgrade increases limit")
        {
            auto upgradeTo =
                static_cast<uint32_t>(xdr::xdr_size(tx2.transaction()));
            SECTION("both upgrade")
            {
                // First increase the limit
                upgradeApp(app1, upgradeTo);
                upgradeApp(app2, upgradeTo);

                // Allow the upgrade to go through, and SEND_MORE messages to be
                // sent
                testutil::crankSome(clock);
                conn.getInitiator()->sendMessage(
                    std::make_shared<StellarMessage>(tx2));
                testutil::crankSome(clock);
                REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
                REQUIRE(txsRecv.count() == start + 2);

                SECTION("fee-bump for tx2 is within limit after upgrade")
                {
                    StellarMessage feeBump2 = makeFeeBump(tx2);
                    testutil::crankSome(clock);
                    conn.getInitiator()->sendMessage(
                        std::make_shared<StellarMessage>(feeBump2));
                    testutil::crankSome(clock);
                    // Fee-bump got sent
                    REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
                    REQUIRE(txsRecv.count() == start + 3);
                }
                SECTION("upgrade decreases limit")
                {
                    // Place another large tx in the queue, then immediately
                    // upgrade to decrease the limit. The transaction should
                    // still go through, but it will be rejected due to the
                    // new size limit
                    conn.getInitiator()->sendMessage(
                        std::make_shared<StellarMessage>(tx2));

                    // We already verified that txSize2 >
                    // MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES
                    upgradeApp(app1,
                               MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES);
                    upgradeApp(app2,
                               MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES);

                    auto& sendMoreMeter = app1->getMetrics().NewMeter(
                        {"overlay", "send", "send-more"}, "message");
                    auto before = sendMoreMeter.count();

                    // Allow upgrade to go through, no SEND_MORE messages are
                    // sent
                    testutil::crankSome(clock);
                    REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
                    REQUIRE(txsRecv.count() == start + 3);

                    REQUIRE(before == sendMoreMeter.count());

                    // Tx1 can still go through due to classic limit
                    conn.getInitiator()->sendMessage(
                        std::make_shared<StellarMessage>(tx1));
                    testutil::crankSome(clock);
                    REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
                    REQUIRE(txsRecv.count() == start + 4);

                    // Tx2 gets dropped
                    conn.getInitiator()->sendMessage(
                        std::make_shared<StellarMessage>(tx2));
                    testutil::crankSome(clock);
                    REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
                    REQUIRE(txsRecv.count() == start + 4);
                }
            }
            SECTION("upgrade delayed")
            {
                // Upgrade initiator, but not acceptor
                // This means the initiator will not drop messages of bigger
                // size, but they'll be stuck in the queue until the acceptor
                // upgrades
                upgradeApp(app1, upgradeTo);

                // Allow the upgrade to go through
                testutil::crankSome(clock);
                conn.getInitiator()->sendMessage(
                    std::make_shared<StellarMessage>(tx2));
                REQUIRE(conn.getInitiator()->getTxQueueByteCount() == txSize2);

                // Still stuck after some time
                testutil::crankSome(clock);
                REQUIRE(conn.getInitiator()->getTxQueueByteCount() == txSize2);
                REQUIRE(txsRecv.count() == start + 1);

                SECTION("acceptor eventually upgrades")
                {
                    // Upgrade acceptor, now the message goes through
                    upgradeApp(app2, upgradeTo);
                    testutil::crankSome(clock);
                    REQUIRE(conn.getInitiator()->getTxQueueByteCount() == 0);
                    REQUIRE(txsRecv.count() == start + 2);
                }
                SECTION("acceptor never upgrades, drop after timeout")
                {
                    testutil::crankFor(clock,
                                       Peer::PEER_SEND_MODE_IDLE_TIMEOUT +
                                           std::chrono::seconds(5));
                    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
                    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
                    REQUIRE(conn.getAcceptor()->getDropReason() ==
                            "idle timeout (no new flood requests)");
                }
            }
        }
    }
}

TEST_CASE("loopback peer flow control activation", "[overlay][flowcontrol]")
{
    VirtualClock clock;
    std::vector<Config> cfgs = {getTestConfig(0), getTestConfig(1)};
    auto cfg1 = cfgs[0];
    auto cfg2 = cfgs[1];
    REQUIRE(cfg1.PEER_FLOOD_READING_CAPACITY !=
            cfg1.PEER_FLOOD_READING_CAPACITY_BYTES);

    auto runTest = [&](std::vector<Config> expectedCfgs,
                       bool sendIllegalSendMore) {
        auto app1 = createTestApplication(clock, expectedCfgs[0]);
        auto app2 = createTestApplication(clock, expectedCfgs[1]);

        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());
        REQUIRE(conn.getInitiator()->checkCapacity(conn.getAcceptor()));
        REQUIRE(conn.getAcceptor()->checkCapacity(conn.getInitiator()));

        // Try sending invalid SEND_MORE with invalid value
        if (sendIllegalSendMore)
        {
            std::string dropReason;
            SECTION("invalid value in the message")
            {
                // Flow control is enabled, ensure it can't be disabled,
                // and the misbehaving peer gets dropped
                conn.getAcceptor()->sendSendMore(0, 0);
                dropReason = "invalid message SEND_MORE_EXTENDED";
            }
            SECTION("invalid message type")
            {
                // Manually construct a SEND_MORE message and send it
                auto m = std::make_shared<StellarMessage>();
                m->type(SEND_MORE);
                m->sendMoreMessage().numMessages = 1;
                conn.getAcceptor()->sendAuthenticatedMessageForTesting(m);
                dropReason = "unexpected message type SEND_MORE";
            }
            testutil::crankSome(clock);
            REQUIRE(!conn.getInitiator()->isConnectedForTesting());
            REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn.getAcceptor()->getDropReason() == dropReason);
        }

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    };

    SECTION("basic")
    {
        // Flow control without illegal SEND_MORE
        runTest({cfg1, cfg2}, false);
    }

    SECTION("bad peer")
    {
        // Flow control with illegal SEND_MORE
        runTest({cfg1, cfg2}, true);
    }
}

TEST_CASE("drop peers that dont respect capacity", "[overlay][flowcontrol]")
{
    VirtualClock clock;
    std::vector<Config> cfgs = {getTestConfig(0), getTestConfig(1)};
    auto cfg1 = cfgs[0];
    auto cfg2 = cfgs[1];

    // tx is invalid, but it doesn't matter
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction().v0().tx.operations.emplace_back(
        getOperationGreaterThanMinMaxSizeBytes());
    uint32 txSize = static_cast<uint32>(xdr::xdr_argpack_size(msg));

    cfg1.PEER_FLOOD_READING_CAPACITY_BYTES =
        txSize + 1 + Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
    cfg1.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = 1;

    auto app1 = createTestApplication(clock, cfg1, true, false);
    auto app2 = createTestApplication(clock, cfg2, true, false);
    app1->getHerder().setMaxClassicTxSize(txSize);
    app2->getHerder().setMaxClassicTxSize(txSize);
    app1->start();
    if (appProtocolVersionStartsFrom(*app1, SOROBAN_PROTOCOL_VERSION))
    {
        modifySorobanNetworkConfig(*app1, [txSize](SorobanNetworkConfig& cfg) {
            cfg.mTxMaxSizeBytes = txSize;
        });
    }

    app2->start();

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);
    REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

    // Acceptor sends too many flood messages, causing initiator to drop it
    auto msgPtr = std::make_shared<StellarMessage>(msg);
    conn.getAcceptor()->sendAuthenticatedMessage(msgPtr);
    conn.getAcceptor()->sendAuthenticatedMessage(msgPtr);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn.getInitiator()->getDropReason() ==
            "unexpected flood message, peer at capacity");

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("drop idle flow-controlled peers", "[overlay][flowcontrol]")
{
    VirtualClock clock;
    std::vector<Config> cfgs = {getTestConfig(0), getTestConfig(1)};
    auto cfg1 = cfgs[0];
    auto cfg2 = cfgs[1];

    StellarMessage msg;
    msg.type(TRANSACTION);
    uint32 txSize = static_cast<uint32>(xdr::xdr_argpack_size(msg));

    cfg1.PEER_FLOOD_READING_CAPACITY_BYTES = txSize;
    // Incorrectly set batch size, so that the node does not send flood
    // requests
    cfg1.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = txSize + 1;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);
    REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

    REQUIRE(conn.getAcceptor()->checkCapacity(conn.getInitiator()));
    // Send outbound message and start the timer
    conn.getAcceptor()->sendMessage(std::make_shared<StellarMessage>(msg),
                                    false);
    conn.getAcceptor()->sendMessage(std::make_shared<StellarMessage>(msg),
                                    false);

    REQUIRE(conn.getAcceptor()
                ->getFlowControl()
                ->getCapacityBytes()
                .getOutboundCapacity() < txSize);

    testutil::crankFor(clock, Peer::PEER_SEND_MODE_IDLE_TIMEOUT +
                                  std::chrono::seconds(5));

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn.getAcceptor()->getDropReason() ==
            "idle timeout (no new flood requests)");

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("drop peers that overflow capacity", "[overlay][flowcontrol]")
{
    VirtualClock clock;
    Config cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);
    REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

    REQUIRE(conn.getInitiator()->checkCapacity(conn.getAcceptor()));
    REQUIRE(conn.getAcceptor()->checkCapacity(conn.getInitiator()));
    auto limit = UINT64_MAX;

    SECTION("message capacity")
    {
        // Set outbound capacity close to max on initiator
        conn.getInitiator()
            ->getFlowControl()
            ->getCapacity()
            .setOutboundCapacity(limit);
    }
    SECTION("byte capacity")
    {
        conn.getInitiator()
            ->getFlowControl()
            ->getCapacityBytes()
            .setOutboundCapacity(limit);
    }

    conn.getAcceptor()->sendSendMore(2, 2);
    testutil::crankFor(clock, std::chrono::seconds(1));

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn.getInitiator()->getDropReason() == "Peer capacity overflow");

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("failed auth", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getInitiator()->setDamageAuth(true);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn.getInitiator()->getDropReason() == "unexpected MAC");

    REQUIRE(knowsAsOutbound(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("peers during auth", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    // Put a peer into Acceptor's DB to trigger sending of peers during auth
    app2->getOverlayManager().getPeerManager().ensureExists(
        PeerBareAddress{"1.1.1.1", 11625});

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
    REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

    StellarMessage newMsg;
    newMsg.type(PEERS);
    std::string dropReason;
    SECTION("inbound")
    {
        dropReason = "received PEERS";
        conn.getInitiator()->sendMessage(
            std::make_shared<StellarMessage>(newMsg));
    }
    SECTION("outbound")
    {
        dropReason = "too many msgs PEERS";
        conn.getAcceptor()->sendMessage(
            std::make_shared<StellarMessage>(newMsg));
    }

    testutil::crankFor(clock, std::chrono::seconds(1));

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn.getAcceptor()->getDropReason() == dropReason);

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("outbound queue filtering", "[overlay][connections]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = std::make_shared<Simulation>(
        Simulation::OVER_LOOPBACK, networkID, [](int i) {
            auto cfg = getTestConfig(i, Config::TESTDB_BUCKET_DB_PERSISTENT);
            cfg.MAX_SLOTS_TO_REMEMBER = 3;
            return cfg;
        });

    auto validatorAKey = SecretKey::fromSeed(sha256("validator-A"));
    auto validatorBKey = SecretKey::fromSeed(sha256("validator-B"));
    auto validatorCKey = SecretKey::fromSeed(sha256("validator-C"));

    SCPQuorumSet qset;
    qset.threshold = 3;
    qset.validators.push_back(validatorAKey.getPublicKey());
    qset.validators.push_back(validatorBKey.getPublicKey());
    qset.validators.push_back(validatorCKey.getPublicKey());

    simulation->addNode(validatorAKey, qset);
    simulation->addNode(validatorBKey, qset);
    simulation->addNode(validatorCKey, qset);

    simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                     validatorCKey.getPublicKey());
    simulation->addPendingConnection(validatorAKey.getPublicKey(),
                                     validatorBKey.getPublicKey());

    simulation->startAllNodes();
    auto node = simulation->getNode(validatorCKey.getPublicKey());

    // Crank some ledgers so that we have SCP messages
    auto ledgers = node->getConfig().MAX_SLOTS_TO_REMEMBER + 1;
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(ledgers, 1); },
        2 * ledgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto conn = simulation->getLoopbackConnection(validatorAKey.getPublicKey(),
                                                  validatorCKey.getPublicKey());
    REQUIRE(conn);
    auto peer = conn->getAcceptor();

    auto& scpQueue = conn->getAcceptor()->getQueues()[0];
    auto& txQueue = conn->getAcceptor()->getQueues()[1];
    auto& demandQueue = conn->getAcceptor()->getQueues()[2];
    auto& advertQueue = conn->getAcceptor()->getQueues()[3];

    // Clear queues for testing
    scpQueue.clear();
    txQueue.clear();
    demandQueue.clear();
    advertQueue.clear();

    auto lcl = node->getLedgerManager().getLastClosedLedgerNum();
    HerderImpl& herder = *static_cast<HerderImpl*>(&node->getHerder());
    auto envs = herder.getSCP().getLatestMessagesSend(lcl);
    REQUIRE(!envs.empty());

    auto constructSCPMsg = [&](SCPEnvelope const& env) {
        StellarMessage msg;
        msg.type(SCP_MESSAGE);
        msg.envelope() = env;
        return std::make_shared<StellarMessage const>(msg);
    };

    auto testTimeBasedTrimming =
        [&](std::deque<FlowControl::QueuedOutboundMessage> const& queue,
            StellarMessage const& msg) {
            peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                std::make_shared<StellarMessage const>(msg));
            REQUIRE(queue.size() == 1);
            simulation->setCurrentVirtualTime(node->getClock().now() +
                                              std::chrono::minutes(2));
        };

    SECTION("SCP messages, slot too old")
    {
        for (auto& env : envs)
        {
            env.statement.slotIndex =
                lcl - node->getConfig().MAX_SLOTS_TO_REMEMBER;
            constructSCPMsg(env);
            peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                constructSCPMsg(env));
        }

        // Always keep most recent checkpoint messages
        REQUIRE(scpQueue.size() == 2);
    }
    SECTION("SCP messages, checkpoint too old")
    {
        // Advance to next checkpoint
        auto nextCheckpoint =
            HistoryManager::firstLedgerAfterCheckpointContaining(
                lcl, node->getConfig());
        simulation->crankUntil(
            [&]() {
                return simulation->haveAllExternalized(nextCheckpoint, 1);
            },
            2 * (nextCheckpoint - lcl) * Herder::EXP_LEDGER_TIMESPAN_SECONDS,
            false);

        envs = herder.getSCP().getLatestMessagesSend(nextCheckpoint);
        auto checkpointFreq =
            HistoryManager::getCheckpointFrequency(node->getConfig());
        for (auto& env : envs)
        {
            env.statement.slotIndex -= checkpointFreq;
            constructSCPMsg(env);
            peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                constructSCPMsg(env));
        }

        // Check that old checkpoint has been deleted
        REQUIRE(scpQueue.empty());
    }
    SECTION("txs, limit reached")
    {
        uint32_t limit = node->getLedgerManager().getLastMaxTxSetSizeOps();
        StellarMessage msg;
        msg.type(TRANSACTION);
        auto byteSize =
            peer->getFlowControl()->getCapacityBytes().getMsgResourceCount(msg);
        SECTION("trim based on message count")
        {
            for (uint32_t i = 0; i < limit + 10; ++i)
            {
                peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                    std::make_shared<StellarMessage const>(msg));
            }

            REQUIRE(peer->getFlowControl()->getTxQueueByteCountForTesting() <
                    peer->getFlowControl()->getOutboundQueueByteLimit());
            REQUIRE(peer->getTxQueueByteCount() == (limit * byteSize));
            REQUIRE(txQueue.size() == limit);
        }
        SECTION("trim time-based")
        {
            for (uint32_t i = 0; i < 10; ++i)
            {
                testTimeBasedTrimming(txQueue, msg);
            }
        }
        SECTION("trim based on byte count")
        {
            // Can fit at most 1 message
            peer->getFlowControl()->setOutboundQueueLimit(byteSize * 3 / 2);

            for (uint32_t i = 0; i < 3; ++i)
            {
                peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                    std::make_shared<StellarMessage const>(msg));
            }

            REQUIRE(peer->getFlowControl()->getTxQueueByteCountForTesting() ==
                    byteSize);
            REQUIRE(txQueue.size() == 1);
        }
    }
    SECTION("obsolete SCP messages")
    {
        SECTION("only latest messages, no trimming")
        {
            // SCP messages aren't affected by the byte limit
            peer->getFlowControl()->setOutboundQueueLimit(1);

            for (auto& env : envs)
            {
                peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                    constructSCPMsg(env));
            }

            // Only latest SCP messages, nothing is trimmed
            REQUIRE(scpQueue.size() == envs.size());
        }
        SECTION("trim obsolete messages")
        {
            auto injectPrepareMsgs = [&](std::vector<SCPEnvelope> envs) {
                for (auto& env : envs)
                {
                    if (env.statement.pledges.type() == SCP_ST_EXTERNALIZE)
                    {
                        // Insert a message that's guaranteed to be older
                        // (prepare vs externalize)
                        auto envCopy = env;
                        envCopy.statement.pledges.type(SCP_ST_PREPARE);

                        peer->getFlowControl()
                            ->addToQueueAndMaybeTrimForTesting(
                                constructSCPMsg(envCopy));
                    }
                    peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                        constructSCPMsg(env));
                }
            };
            SECTION("trim prepare, keep nomination")
            {
                injectPrepareMsgs(envs);

                // prepare got dropped
                REQUIRE(scpQueue.size() == 2);
                REQUIRE(
                    scpQueue[0].mMessage->envelope().statement.pledges.type() ==
                    SCP_ST_NOMINATE);
                REQUIRE(
                    scpQueue[1].mMessage->envelope().statement.pledges.type() ==
                    SCP_ST_EXTERNALIZE);
            }
            SECTION("trim prepare, keep messages from other nodes")
            {
                // Get ballot protocol messages from all nodes
                auto msgs = herder.getSCP().getExternalizingState(lcl);
                auto hintMsg = msgs.back();
                injectPrepareMsgs(msgs);

                // 3 externalize messages remaining
                REQUIRE(scpQueue.size() == 3);
                REQUIRE(std::all_of(scpQueue.begin(), scpQueue.end(),
                                    [&](auto const& item) {
                                        return item.mMessage->envelope()
                                                   .statement.pledges.type() ==
                                               SCP_ST_EXTERNALIZE;
                                    }));
            }
        }
    }
    SECTION("advert demand limit reached")
    {
        SECTION("count-based")
        {
            // Adverts/demands aren't affected by the byte limit
            peer->getFlowControl()->setOutboundQueueLimit(1);
            uint32_t limit = node->getLedgerManager().getLastMaxTxSetSizeOps();
            for (uint32_t i = 0; i < limit + 10; ++i)
            {
                StellarMessage adv, dem, txn;
                adv.type(FLOOD_ADVERT);
                dem.type(FLOOD_DEMAND);
                adv.floodAdvert().txHashes.push_back(xdrSha256(txn));
                dem.floodDemand().txHashes.push_back(xdrSha256(txn));
                peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                    std::make_shared<StellarMessage const>(adv));
                peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                    std::make_shared<StellarMessage const>(dem));
            }

            REQUIRE(advertQueue.size() == limit);
            REQUIRE(demandQueue.size() == limit);

            StellarMessage adv, dem, txn;
            adv.type(FLOOD_ADVERT);
            dem.type(FLOOD_DEMAND);
            for (auto i = 0; i < 2; i++)
            {
                adv.floodAdvert().txHashes.push_back(xdrSha256(txn));
                dem.floodDemand().txHashes.push_back(xdrSha256(txn));
            }

            peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                std::make_shared<StellarMessage const>(adv));
            peer->getFlowControl()->addToQueueAndMaybeTrimForTesting(
                std::make_shared<StellarMessage const>(dem));

            REQUIRE(advertQueue.size() == limit - 1);
            REQUIRE(demandQueue.size() == limit - 1);
        }
        SECTION("time-based")
        {
            Hash hash;
            advertQueue.clear();
            demandQueue.clear();
            for (uint32_t i = 0; i < 10; ++i)
            {
                StellarMessage adv, dem;
                adv.type(FLOOD_ADVERT);
                adv.floodAdvert().txHashes.push_back(hash);
                testTimeBasedTrimming(advertQueue, adv);

                dem.type(FLOOD_DEMAND);
                dem.floodDemand().txHashes.push_back(hash);
                testTimeBasedTrimming(demandQueue, dem);
            }
        }
    }
}

TEST_CASE("reject non preferred peer", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.PREFERRED_PEERS_ONLY = true;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    SECTION("inbound")
    {
        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(!conn.getInitiator()->isConnectedForTesting());
        REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
        REQUIRE(conn.getAcceptor()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsOutbound(*app1, *app2));
        REQUIRE(knowsAsInbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }

    SECTION("outbound")
    {
        LoopbackPeerConnection conn(*app2, *app1);
        testutil::crankSome(clock);

        REQUIRE(!conn.getInitiator()->isConnectedForTesting());
        REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
        REQUIRE(conn.getInitiator()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsInbound(*app1, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }
}

TEST_CASE("accept preferred peer even when strict", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.PREFERRED_PEERS_ONLY = true;
    cfg2.PREFERRED_PEER_KEYS.emplace(cfg1.NODE_SEED.getPublicKey());

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    SECTION("inbound")
    {
        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

        REQUIRE(knowsAsOutbound(*app1, *app2));
        REQUIRE(knowsAsInbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }

    SECTION("outbound")
    {
        LoopbackPeerConnection conn(*app2, *app1);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

        REQUIRE(knowsAsInbound(*app1, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }
}

TEST_CASE("reject peers beyond max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    SECTION("inbound")
    {
        cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 1;
        cfg2.TARGET_PEER_CONNECTIONS = 0;

        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);
        auto app3 = createTestApplication(clock, cfg3);

        LoopbackPeerConnection conn1(*app1, *app2);
        LoopbackPeerConnection conn2(*app3, *app2);
        testutil::crankSome(clock);

        REQUIRE(conn1.getInitiator()->isConnectedForTesting());
        REQUIRE(conn1.getAcceptor()->isConnectedForTesting());
        REQUIRE(!conn2.getInitiator()->isConnectedForTesting());
        REQUIRE(!conn2.getAcceptor()->isConnectedForTesting());
        REQUIRE(conn2.getAcceptor()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsOutbound(*app1, *app2));
        REQUIRE(knowsAsInbound(*app2, *app1));
        REQUIRE(knowsAsOutbound(*app3, *app2));
        REQUIRE(knowsAsInbound(*app2, *app3));

        testutil::shutdownWorkScheduler(*app3);
        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }

    SECTION("outbound")
    {
        cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
        cfg2.TARGET_PEER_CONNECTIONS = 1;

        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);
        auto app3 = createTestApplication(clock, cfg3);

        LoopbackPeerConnection conn1(*app2, *app1);
        LoopbackPeerConnection conn2(*app2, *app3);
        testutil::crankSome(clock);

        REQUIRE(conn1.getInitiator()->isConnectedForTesting());
        REQUIRE(conn1.getAcceptor()->isConnectedForTesting());
        REQUIRE(!conn2.getInitiator()->isConnectedForTesting());
        REQUIRE(!conn2.getAcceptor()->isConnectedForTesting());
        REQUIRE(conn2.getInitiator()->getDropReason() == "peer rejected");

        REQUIRE(knowsAsInbound(*app1, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app1));
        REQUIRE(knowsAsInbound(*app3, *app2));
        REQUIRE(knowsAsOutbound(*app2, *app3));

        testutil::shutdownWorkScheduler(*app3);
        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    }
}

TEST_CASE("reject peers beyond max - preferred peer wins",
          "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);

    SECTION("preferred connects first")
    {
        SECTION("inbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 1;
            cfg2.TARGET_PEER_CONNECTIONS = 0;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn2(*app3, *app2);
            LoopbackPeerConnection conn1(*app1, *app2);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnectedForTesting());
            REQUIRE(!conn1.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn2.getInitiator()->isConnectedForTesting());
            REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn1.getAcceptor()->getDropReason() == "peer rejected");

            REQUIRE(knowsAsOutbound(*app1, *app2));
            REQUIRE(knowsAsInbound(*app2, *app1));
            REQUIRE(knowsAsOutbound(*app3, *app2));
            REQUIRE(knowsAsInbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }

        SECTION("outbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
            cfg2.TARGET_PEER_CONNECTIONS = 1;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn2(*app2, *app3);
            LoopbackPeerConnection conn1(*app2, *app1);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnectedForTesting());
            REQUIRE(!conn1.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn2.getInitiator()->isConnectedForTesting());
            REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn1.getInitiator()->getDropReason() == "peer rejected");

            REQUIRE(knowsAsInbound(*app1, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app1));
            REQUIRE(knowsAsInbound(*app3, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }
    }

    SECTION("preferred connects second")
    {
        SECTION("inbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 1;
            cfg2.TARGET_PEER_CONNECTIONS = 0;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn1(*app1, *app2);
            LoopbackPeerConnection conn2(*app3, *app2);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnectedForTesting());
            REQUIRE(!conn1.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn2.getInitiator()->isConnectedForTesting());
            REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn1.getAcceptor()->getDropReason() ==
                    "preferred peer selected instead");

            REQUIRE(knowsAsOutbound(*app1, *app2));
            REQUIRE(knowsAsInbound(*app2, *app1));
            REQUIRE(knowsAsOutbound(*app3, *app2));
            REQUIRE(knowsAsInbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }

        SECTION("outbound")
        {
            cfg2.MAX_ADDITIONAL_PEER_CONNECTIONS = 0;
            cfg2.TARGET_PEER_CONNECTIONS = 1;
            cfg2.PREFERRED_PEER_KEYS.emplace(cfg3.NODE_SEED.getPublicKey());

            auto app1 = createTestApplication(clock, cfg1);
            auto app2 = createTestApplication(clock, cfg2);
            auto app3 = createTestApplication(clock, cfg3);

            LoopbackPeerConnection conn1(*app2, *app1);
            LoopbackPeerConnection conn2(*app2, *app3);
            testutil::crankSome(clock);

            REQUIRE(!conn1.getInitiator()->isConnectedForTesting());
            REQUIRE(!conn1.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn2.getInitiator()->isConnectedForTesting());
            REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
            REQUIRE(conn1.getInitiator()->getDropReason() ==
                    "preferred peer selected instead");

            REQUIRE(knowsAsInbound(*app1, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app1));
            REQUIRE(knowsAsInbound(*app3, *app2));
            REQUIRE(knowsAsOutbound(*app2, *app3));

            testutil::shutdownWorkScheduler(*app3);
            testutil::shutdownWorkScheduler(*app2);
            testutil::shutdownWorkScheduler(*app1);
        }
    }
}

TEST_CASE("allow inbound pending peers up to max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);
    Config const& cfg5 = getTestConfig(4);

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 3;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 3;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);
    auto app5 = createTestApplication(clock, cfg5);

    LoopbackPeerConnection conn1(*app1, *app2);
    REQUIRE(conn1.getInitiator()->isConnectedForTesting());
    REQUIRE(conn1.getAcceptor()->isConnectedForTesting());
    conn1.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn2(*app3, *app2);
    REQUIRE(conn2.getInitiator()->isConnectedForTesting());
    REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
    conn2.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn3(*app4, *app2);
    REQUIRE(conn3.getInitiator()->isConnectedForTesting());
    REQUIRE(conn3.getAcceptor()->isConnectedForTesting());

    LoopbackPeerConnection conn4(*app5, *app2);
    REQUIRE(conn4.getInitiator()->isConnectedForTesting());
    REQUIRE(conn4.getAcceptor()->shouldAbortForTesting());

    // Must wait for RECURRENT_TIMER_PERIOD
    testutil::crankFor(clock, std::chrono::seconds(5));

    REQUIRE(conn1.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn1.getAcceptor()->shouldAbortForTesting());
    REQUIRE(conn2.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn2.getAcceptor()->shouldAbortForTesting());
    REQUIRE(conn3.getInitiator()->isConnectedForTesting());
    REQUIRE(conn3.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn4.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn4.getAcceptor()->shouldAbortForTesting());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);

    REQUIRE(doesNotKnow(*app1, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app1)); // corked
    REQUIRE(doesNotKnow(*app3, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app3)); // corked
    REQUIRE(knowsAsOutbound(*app4, *app2));
    REQUIRE(knowsAsInbound(*app2, *app4));
    REQUIRE(doesNotKnow(*app5, *app2)); // didn't get to hello phase
    REQUIRE(doesNotKnow(*app2, *app5)); // didn't get to hello phase

    testutil::shutdownWorkScheduler(*app5);
    testutil::shutdownWorkScheduler(*app4);
    testutil::shutdownWorkScheduler(*app3);
    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("allow inbound pending peers over max if possibly preferred",
          "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);
    Config const& cfg5 = getTestConfig(4);

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 3;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 3;
    cfg2.PREFERRED_PEERS.emplace_back("127.0.0.1:17");

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);
    auto app5 = createTestApplication(clock, cfg5);

    (static_cast<OverlayManagerImpl&>(app2->getOverlayManager()))
        .storeConfigPeers();

    LoopbackPeerConnection conn1(*app1, *app2);
    REQUIRE(conn1.getInitiator()->isConnectedForTesting());
    REQUIRE(conn1.getAcceptor()->isConnectedForTesting());
    conn1.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn2(*app3, *app2);
    REQUIRE(conn2.getInitiator()->isConnectedForTesting());
    REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
    conn2.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn3(*app4, *app2);
    REQUIRE(conn3.getInitiator()->isConnectedForTesting());
    REQUIRE(conn3.getAcceptor()->isConnectedForTesting());

    LoopbackPeerConnection conn4(*app5, *app2);
    REQUIRE(conn4.getInitiator()->isConnectedForTesting());
    REQUIRE(conn4.getAcceptor()->isConnectedForTesting());

    // Must wait for RECURRENT_TIMER_PERIOD
    testutil::crankFor(clock, std::chrono::seconds(5));

    REQUIRE(conn1.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn1.getAcceptor()->shouldAbortForTesting());
    REQUIRE(conn2.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn2.getAcceptor()->shouldAbortForTesting());
    REQUIRE(conn3.getInitiator()->isConnectedForTesting());
    REQUIRE(conn3.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn4.getInitiator()->isConnectedForTesting());
    REQUIRE(conn4.getAcceptor()->isConnectedForTesting());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "connection", "reject"}, "connection")
                .count() == 0);

    REQUIRE(doesNotKnow(*app1, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app1)); // corked
    REQUIRE(doesNotKnow(*app3, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app3)); // corked
    REQUIRE(knowsAsOutbound(*app4, *app2));
    REQUIRE(knowsAsInbound(*app2, *app4));
    REQUIRE(knowsAsOutbound(*app5, *app2));
    REQUIRE(knowsAsInbound(*app2, *app5));

    testutil::shutdownWorkScheduler(*app5);
    testutil::shutdownWorkScheduler(*app4);
    testutil::shutdownWorkScheduler(*app3);
    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("allow outbound pending peers up to max", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);
    Config const& cfg3 = getTestConfig(2);
    Config const& cfg4 = getTestConfig(3);
    Config const& cfg5 = getTestConfig(4);

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 3;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 3;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    auto app3 = createTestApplication(clock, cfg3);
    auto app4 = createTestApplication(clock, cfg4);
    auto app5 = createTestApplication(clock, cfg5);

    LoopbackPeerConnection conn1(*app2, *app1);
    REQUIRE(conn1.getInitiator()->isConnectedForTesting());
    REQUIRE(conn1.getAcceptor()->isConnectedForTesting());
    conn1.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn2(*app2, *app3);
    REQUIRE(conn2.getInitiator()->isConnectedForTesting());
    REQUIRE(conn2.getAcceptor()->isConnectedForTesting());
    conn2.getInitiator()->setCorked(true);

    LoopbackPeerConnection conn3(*app2, *app4);
    REQUIRE(conn3.getInitiator()->isConnectedForTesting());
    REQUIRE(conn3.getAcceptor()->isConnectedForTesting());

    LoopbackPeerConnection conn4(*app2, *app5);
    REQUIRE(conn4.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn4.getAcceptor()->isConnectedForTesting());
    conn2.getInitiator()->setCorked(true);

    // Must wait for RECURRENT_TIMER_PERIOD
    testutil::crankFor(clock, std::chrono::seconds(5));

    REQUIRE(conn1.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn1.getAcceptor()->shouldAbortForTesting());
    REQUIRE(conn2.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn2.getAcceptor()->shouldAbortForTesting());
    REQUIRE(conn3.getInitiator()->isConnectedForTesting());
    REQUIRE(conn3.getAcceptor()->isConnectedForTesting());
    REQUIRE(conn4.getInitiator()->shouldAbortForTesting());
    REQUIRE(conn4.getAcceptor()->shouldAbortForTesting());
    REQUIRE(app2->getMetrics()
                .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                .count() == 2);

    REQUIRE(doesNotKnow(*app1, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app1)); // corked
    REQUIRE(doesNotKnow(*app3, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app3)); // corked
    REQUIRE(knowsAsInbound(*app4, *app2));
    REQUIRE(knowsAsOutbound(*app2, *app4));
    REQUIRE(doesNotKnow(*app5, *app2)); // corked
    REQUIRE(doesNotKnow(*app2, *app5)); // corked

    testutil::shutdownWorkScheduler(*app5);
    testutil::shutdownWorkScheduler(*app4);
    testutil::shutdownWorkScheduler(*app3);
    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("reject peers with differing network passphrases",
          "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    cfg2.NETWORK_PASSPHRASE = "nothing to see here";

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(doesNotKnow(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("reject peers with invalid cert", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    LoopbackPeerConnection conn(*app1, *app2);
    conn.getAcceptor()->setDamageCert(true);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("reject banned peers", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(0);
    Config cfg2 = getTestConfig(1);

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
    app1->getBanManager().banNode(cfg2.NODE_SEED.getPublicKey());

    LoopbackPeerConnection conn(*app1, *app2);
    testutil::crankSome(clock);

    REQUIRE(!conn.getInitiator()->isConnectedForTesting());
    REQUIRE(!conn.getAcceptor()->isConnectedForTesting());

    REQUIRE(doesNotKnow(*app1, *app2));
    REQUIRE(knowsAsInbound(*app2, *app1));

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("reject peers with incompatible overlay versions",
          "[overlay][connections]")
{
    Config const& cfg1 = getTestConfig(0);

    auto doVersionCheck = [&](uint32 version) {
        VirtualClock clock;
        Config cfg2 = getTestConfig(1);

        cfg2.OVERLAY_PROTOCOL_MIN_VERSION = version;
        cfg2.OVERLAY_PROTOCOL_VERSION = version;
        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);

        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(!conn.getInitiator()->isConnectedForTesting());
        REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
        REQUIRE(conn.getInitiator()->getDropReason() ==
                "wrong protocol version");

        REQUIRE(doesNotKnow(*app1, *app2));
        REQUIRE(doesNotKnow(*app2, *app1));

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    };
    SECTION("cfg2 above")
    {
        doVersionCheck(cfg1.OVERLAY_PROTOCOL_VERSION + 1);
    }
    SECTION("cfg2 below")
    {
        doVersionCheck(cfg1.OVERLAY_PROTOCOL_MIN_VERSION - 1);
    }
}

TEST_CASE("reject peers who dont handshake quickly", "[overlay][connections]")
{
    auto test = [](unsigned short authenticationTimeout) {
        Config cfg1 = getTestConfig(1);
        Config cfg2 = getTestConfig(2);

        cfg1.PEER_AUTHENTICATION_TIMEOUT = authenticationTimeout;
        cfg2.PEER_AUTHENTICATION_TIMEOUT = authenticationTimeout;

        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto sim =
            std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

        SIMULATION_CREATE_NODE(Node1);
        SIMULATION_CREATE_NODE(Node2);
        sim->addNode(vNode1SecretKey, cfg1.QUORUM_SET, &cfg1);
        sim->addNode(vNode2SecretKey, cfg2.QUORUM_SET, &cfg2);
        auto waitTime = std::chrono::seconds(authenticationTimeout + 1);
        auto padTime = std::chrono::seconds(2);

        sim->addPendingConnection(vNode1NodeID, vNode2NodeID);

        sim->startAllNodes();

        auto conn = sim->getLoopbackConnection(vNode1NodeID, vNode2NodeID);

        conn->getInitiator()->setCorked(true);

        sim->crankForAtLeast(waitTime + padTime, false);

        sim->crankUntil(
            [&]() {
                return !(conn->getInitiator()->isConnectedForTesting() ||
                         conn->getAcceptor()->isConnectedForTesting());
            },
            padTime, true);

        auto app1 = sim->getNode(vNode1NodeID);
        auto app2 = sim->getNode(vNode2NodeID);

        auto idle1 = app1->getMetrics()
                         .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                         .count();
        auto idle2 = app2->getMetrics()
                         .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                         .count();

        REQUIRE((idle1 != 0 || idle2 != 0));

        REQUIRE(doesNotKnow(*app1, *app2));
        REQUIRE(doesNotKnow(*app2, *app1));
    };

    SECTION("2 seconds timeout")
    {
        test(2);
    }

    SECTION("5 seconds timeout")
    {
        test(5);
    }
}

TEST_CASE("drop peers who straggle", "[overlay][connections][straggler]")
{
    auto test = [](unsigned short stragglerTimeout) {
        VirtualClock clock;
        Config cfg1 = getTestConfig(0);
        Config cfg2 = getTestConfig(1);

        // Straggler detection piggy-backs on the idle timer so we drive
        // the test from idle-timer-firing granularity.
        assert(cfg1.PEER_TIMEOUT == cfg2.PEER_TIMEOUT);
        assert(stragglerTimeout >= cfg1.PEER_TIMEOUT * 2);

        // Initiator (cfg1) will straggle, and acceptor (cfg2) will notice and
        // disconnect.
        cfg2.PEER_STRAGGLER_TIMEOUT = stragglerTimeout;

        auto app1 = createTestApplication(clock, cfg1);
        auto app2 = createTestApplication(clock, cfg2);
        auto waitTime = std::chrono::seconds(stragglerTimeout * 3);
        auto padTime = std::chrono::seconds(5);

        LoopbackPeerConnection conn(*app1, *app2);
        auto start = clock.now();

        testutil::crankSome(clock);
        REQUIRE(conn.getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn.getAcceptor()->isAuthenticatedForTesting());

        conn.getInitiator()->setStraggling(true);
        auto straggler = conn.getInitiator();
        VirtualTimer sendTimer(*app1);

        while (clock.now() < (start + waitTime) &&
               (conn.getInitiator()->isConnectedForTesting() ||
                conn.getAcceptor()->isConnectedForTesting()))
        {
            // Straggler keeps asking for peers once per second -- this is
            // easy traffic to fake-generate -- but not accepting response
            // messages in a timely fashion.
            std::chrono::seconds const dur{1};
            sendTimer.expires_from_now(dur);
            sendTimer.async_wait([straggler](asio::error_code const& error) {
                if (!error)
                {
                    straggler->sendGetTxSet(Hash());
                }
            });
            testutil::crankFor(clock, dur);
        }
        LOG_INFO(DEFAULT_LOG, "loop complete, clock.now() = {}",
                 clock.now().time_since_epoch().count());
        REQUIRE(clock.now() < (start + waitTime + padTime));
        REQUIRE(!conn.getInitiator()->isConnectedForTesting());
        REQUIRE(!conn.getAcceptor()->isConnectedForTesting());
        REQUIRE(app1->getMetrics()
                    .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                    .count() == 0);
        REQUIRE(app2->getMetrics()
                    .NewMeter({"overlay", "timeout", "idle"}, "timeout")
                    .count() == 0);
        REQUIRE(app2->getMetrics()
                    .NewMeter({"overlay", "timeout", "straggler"}, "timeout")
                    .count() != 0);

        testutil::shutdownWorkScheduler(*app2);
        testutil::shutdownWorkScheduler(*app1);
    };

    SECTION("60 seconds straggle timeout")
    {
        test(60);
    }

    SECTION("120 seconds straggle timeout")
    {
        test(120);
    }

    SECTION("150 seconds straggle timeout")
    {
        test(150);
    }
}

TEST_CASE("reject peers with the same nodeid", "[overlay][connections]")
{
    VirtualClock clock;
    Config const& cfg1 = getTestConfig(1);
    Config cfg2 = getTestConfig(2);

    cfg2.NODE_SEED = cfg1.NODE_SEED;

    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);

    SECTION("inbound")
    {
        LoopbackPeerConnection conn(*app1, *app2);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->getDropReason() == "connecting to self");
    }

    SECTION("outbound")
    {
        LoopbackPeerConnection conn(*app2, *app1);
        testutil::crankSome(clock);

        REQUIRE(conn.getInitiator()->getDropReason() == "connecting to self");
    }

    testutil::shutdownWorkScheduler(*app2);
    testutil::shutdownWorkScheduler(*app1);
}

TEST_CASE("connecting to saturated nodes", "[overlay][connections][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    auto getConfiguration = [](int id, unsigned short targetOutboundConnections,
                               unsigned short maxInboundConnections) {
        auto cfg = getTestConfig(id);
        cfg.TARGET_PEER_CONNECTIONS = targetOutboundConnections;
        cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = maxInboundConnections;
        return cfg;
    };

    auto headCfg = getConfiguration(1, 0, 1);
    auto node1Cfg = getConfiguration(2, 1, 1);
    auto node2Cfg = getConfiguration(3, 1, 1);
    auto node3Cfg = getConfiguration(4, 1, 1);

    SIMULATION_CREATE_NODE(Head);
    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(vHeadNodeID);
    qSet.validators.push_back(vNode1NodeID);
    qSet.validators.push_back(vNode2NodeID);
    qSet.validators.push_back(vNode3NodeID);

    auto headId = simulation->addNode(vHeadSecretKey, qSet, &headCfg)
                      ->getConfig()
                      .NODE_SEED.getPublicKey();

    simulation->addNode(vNode1SecretKey, qSet, &node1Cfg);

    // large timeout here as nodes may have a few bad attempts
    // (crossed connections) and we rely on jittered backoffs
    // to mitigate this

    simulation->addPendingConnection(vNode1NodeID, vHeadNodeID);
    simulation->startAllNodes();
    UNSCOPED_INFO("1 connects to h");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections(simulation) == 2; },
        std::chrono::seconds{3}, false);

    simulation->addNode(vNode2SecretKey, qSet, &node2Cfg);
    simulation->addPendingConnection(vNode2NodeID, vHeadNodeID);
    simulation->startAllNodes();
    UNSCOPED_INFO("2 connects to 1");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections(simulation) == 4; },
        std::chrono::seconds{20}, false);

    simulation->addNode(vNode3SecretKey, qSet, &node3Cfg);
    simulation->addPendingConnection(vNode3NodeID, vHeadNodeID);
    simulation->startAllNodes();
    UNSCOPED_INFO("3 connects to 2");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections(simulation) == 6; },
        std::chrono::seconds{30}, false);

    simulation->removeNode(headId);
    UNSCOPED_INFO("wait for node to be disconnected");
    simulation->crankForAtLeast(std::chrono::seconds{2}, false);
    UNSCOPED_INFO("wait for 1 to connect to 3");
    simulation->crankUntil(
        [&]() { return numberOfSimulationConnections(simulation) == 6; },
        std::chrono::seconds{30}, true);
}

TEST_CASE("inbounds nodes can be promoted to ouboundvalid",
          "[overlay][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    auto nodes = std::vector<Application::pointer>{};
    auto configs = std::vector<Config>{};
    auto addresses = std::vector<PeerBareAddress>{};
    for (auto i = 0; i < 3; i++)
    {
        configs.push_back(getTestConfig(i + 1));
        addresses.emplace_back("127.0.0.1", configs[i].PEER_PORT);
    }

    configs[0].KNOWN_PEERS.emplace_back(
        fmt::format("127.0.0.1:{}", configs[1].PEER_PORT));
    configs[2].KNOWN_PEERS.emplace_back(
        fmt::format("127.0.0.1:{}", configs[0].PEER_PORT));

    nodes.push_back(simulation->addNode(vNode1SecretKey, qSet, &configs[0]));
    nodes.push_back(simulation->addNode(vNode2SecretKey, qSet, &configs[1]));
    nodes.push_back(simulation->addNode(vNode3SecretKey, qSet, &configs[2]));

    enum class TestPeerType
    {
        ANY,
        KNOWN,
        OUTBOUND
    };

    auto getTestPeerType = [&](size_t i, size_t j) {
        auto& node = nodes[i];
        auto peer =
            node->getOverlayManager().getPeerManager().load(addresses[j]);
        if (!peer.second)
        {
            return TestPeerType::ANY;
        }

        return peer.first.mType == static_cast<int>(PeerType::INBOUND)
                   ? TestPeerType::KNOWN
                   : TestPeerType::OUTBOUND;
    };

    using ExpectedResultType = std::vector<std::vector<TestPeerType>>;
    auto peerTypesMatch = [&](ExpectedResultType expected) {
        for (size_t i = 0; i < expected.size(); i++)
        {
            for (size_t j = 0; j < expected[i].size(); j++)
            {
                if (expected[i][j] > getTestPeerType(i, j))
                {
                    return false;
                }
            }
        }
        return true;
    };

    simulation->startAllNodes();

    // at first, nodes only know about KNOWN_PEERS
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::KNOWN, TestPeerType::ANY},
                 {TestPeerType::ANY, TestPeerType::ANY, TestPeerType::ANY},
                 {TestPeerType::KNOWN, TestPeerType::ANY, TestPeerType::ANY}});
        },
        std::chrono::seconds(2), false);

    // then, after connection, some are made OUTBOUND
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::OUTBOUND,
                  TestPeerType::KNOWN},
                 {TestPeerType::KNOWN, TestPeerType::ANY, TestPeerType::ANY},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY,
                  TestPeerType::ANY}});
        },
        std::chrono::seconds(10), false);

    // then, after promotion, more are made OUTBOUND
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::OUTBOUND,
                  TestPeerType::OUTBOUND},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY, TestPeerType::ANY},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY,
                  TestPeerType::ANY}});
        },
        std::chrono::seconds(30), false);

    // and when all connections are made, all nodes know about each other
    simulation->crankUntil(
        [&] {
            return peerTypesMatch(
                {{TestPeerType::ANY, TestPeerType::OUTBOUND,
                  TestPeerType::OUTBOUND},
                 {TestPeerType::OUTBOUND, TestPeerType::ANY,
                  TestPeerType::OUTBOUND},
                 {TestPeerType::OUTBOUND, TestPeerType::OUTBOUND,
                  TestPeerType::ANY}});
        },
        std::chrono::seconds(30), false);

    simulation->crankForAtLeast(std::chrono::seconds{3}, true);
}

TEST_CASE("flow control when out of sync", "[overlay][flowcontrol]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    auto configs = std::vector<Config>{};
    for (auto i = 0; i < 2; i++)
    {
        auto cfg = getTestConfig(i + 1);
        cfg.PEER_FLOOD_READING_CAPACITY = 1;
        cfg.PEER_READING_CAPACITY = 1;
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE = 1;
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
        if (i == 1)
        {
            cfg.FORCE_SCP = false;
        }
        configs.push_back(cfg);
    }

    auto node = simulation->addNode(vNode1SecretKey, qSet, &configs[0]);
    auto outOfSyncNode =
        simulation->addNode(vNode2SecretKey, qSet, &configs[1]);
    simulation->startAllNodes();

    // Node1 closes a few ledgers, while Node2 falls behind and goes out of sync
    simulation->crankUntil(
        [&]() {
            return node->getLedgerManager().getLastClosedLedgerNum() >= 15;
        },
        50 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    REQUIRE(!outOfSyncNode->getLedgerManager().isSynced());
    simulation->addConnection(vNode2NodeID, vNode1NodeID);

    // Generate transactions traffic, which the out of sync node will drop
    auto& loadGen = node->getLoadGenerator();
    loadGen.generateLoad(
        GeneratedLoadConfig::createAccountsLoad(/* nAccounts */ 3000,
                                                /* txRate */ 1));

    auto& loadGenDone =
        node->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    auto currLoadGenCount = loadGenDone.count();

    simulation->crankUntil(
        [&]() { return loadGenDone.count() > currLoadGenCount; },
        200 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Confirm Node2 is still connected to Node1 and did not get dropped
    auto conn = simulation->getLoopbackConnection(vNode2NodeID, vNode1NodeID);
    REQUIRE(conn);
    REQUIRE(conn->getInitiator()->isConnectedForTesting());
    REQUIRE(conn->getAcceptor()->isConnectedForTesting());
}

TEST_CASE("overlay flow control", "[overlay][flowcontrol][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    SIMULATION_CREATE_NODE(Node3);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(vNode1NodeID);
    qSet.validators.push_back(vNode2NodeID);
    qSet.validators.push_back(vNode3NodeID);

    auto configs = std::vector<Config>{};

    for (auto i = 0; i < 3; i++)
    {
        auto cfg = getTestConfig(i + 1);

        // Set flow control parameters to something very small
        cfg.PEER_FLOOD_READING_CAPACITY = 1;
        cfg.PEER_READING_CAPACITY = 1;
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE = 1;
        cfg.PEER_FLOOD_READING_CAPACITY_BYTES =
            MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES + 100 +
            Herder::FLOW_CONTROL_BYTES_EXTRA_BUFFER;
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES = 100;
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
        configs.push_back(cfg);
    }

    Application::pointer node = nullptr;
    auto setupSimulation = [&]() {
        node = simulation->addNode(vNode1SecretKey, qSet, &configs[0]);
        auto a1 = simulation->addNode(vNode2SecretKey, qSet, &configs[1]);
        auto a2 = simulation->addNode(vNode3SecretKey, qSet, &configs[2]);
        node->getHerder().setMaxClassicTxSize(5900);
        a1->getHerder().setMaxClassicTxSize(5900);
        a2->getHerder().setMaxClassicTxSize(5900);

        simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
        simulation->addPendingConnection(vNode2NodeID, vNode3NodeID);
        simulation->addPendingConnection(vNode3NodeID, vNode1NodeID);

        simulation->startAllNodes();
        if (appProtocolVersionStartsFrom(*simulation->getNodes()[0],
                                         SOROBAN_PROTOCOL_VERSION))
        {
            upgradeSorobanNetworkConfig(
                [](SorobanNetworkConfig& cfg) {
                    cfg.mTxMaxSizeBytes =
                        MinimumSorobanNetworkConfig::TX_MAX_SIZE_BYTES;
                },
                simulation);
        }
    };

    SECTION("enabled")
    {
        setupSimulation();

        // Generate a bit of load to flood transactions, make sure nodes can
        // close ledgers properly
        auto& loadGen = node->getLoadGenerator();
        loadGen.generateLoad(
            GeneratedLoadConfig::createAccountsLoad(/* nAccounts */ 150,
                                                    /* txRate */ 1));

        auto& loadGenDone =
            node->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
        auto currLoadGenCount = loadGenDone.count();

        simulation->crankUntil(
            [&]() { return loadGenDone.count() > currLoadGenCount; },
            15 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        currLoadGenCount = loadGenDone.count();

        loadGen.generateLoad(GeneratedLoadConfig::txLoad(
            LoadGenMode::PAY, /* nAccounts */ 150, 200,
            /*txRate*/ 5));

        simulation->crankUntil(
            [&]() { return loadGenDone.count() > currLoadGenCount; },
            100 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
    }
    SECTION("do not accept peers without flow control")
    {
        configs[2].PEER_FLOOD_READING_CAPACITY = 0;
        configs[2].PEER_FLOOD_READING_CAPACITY_BYTES = 0;
        REQUIRE_THROWS_AS(setupSimulation(), std::runtime_error);
    }
}

PeerBareAddress
localhost(unsigned short port)
{
    return PeerBareAddress{"127.0.0.1", port};
}

TEST_CASE("database is purged at overlay start", "[overlay]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.RUN_STANDALONE = false;
    auto app = createTestApplication(clock, cfg, true, false);
    auto& om = app->getOverlayManager();
    auto& peerManager = om.getPeerManager();
    auto record = [](size_t numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    // Need to set max tx size on tests that start OverlayManager without
    // starting Herder. Otherwise, flow control initialization will trigger an
    // assertion failure.
    Herder& herder = app->getHerder();
    herder.setMaxTxSize(herder.getMaxClassicTxSize());

    peerManager.store(localhost(1), record(118), false);
    peerManager.store(localhost(2), record(119), false);
    peerManager.store(localhost(3), record(120), false);
    peerManager.store(localhost(4), record(121), false);
    peerManager.store(localhost(5), record(122), false);

    om.start();

    // Must wait 2 seconds as `OverlayManagerImpl::start()`
    // sets a 2-second timer.
    // `crankSome` may not work if other timers fire before that.
    // (e.g., pull-mode advert timer)
    testutil::crankFor(clock, std::chrono::seconds(2));

    REQUIRE(peerManager.load(localhost(1)).second);
    REQUIRE(peerManager.load(localhost(2)).second);
    REQUIRE(!peerManager.load(localhost(3)).second);
    REQUIRE(!peerManager.load(localhost(4)).second);
    REQUIRE(!peerManager.load(localhost(5)).second);

    om.shutdown();
    // Allow shutdown to go through
    testutil::crankFor(clock, std::chrono::seconds(2));
}

TEST_CASE("peer numfailures resets after good connection",
          "[overlay][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);
    auto record = [](size_t numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    Config const& cfg1 = getTestConfig(1);
    Config const& cfg2 = getTestConfig(2);

    auto app1 = simulation->addNode(vNode1SecretKey, qSet, &cfg1);
    auto app2 = simulation->addNode(vNode2SecretKey, qSet, &cfg2);

    simulation->startAllNodes();

    auto& om = app1->getOverlayManager();
    auto& peerManager = om.getPeerManager();
    peerManager.store(localhost(cfg2.PEER_PORT), record(119), false);
    REQUIRE(peerManager.load(localhost(cfg2.PEER_PORT)).second);

    simulation->crankForAtLeast(std::chrono::seconds{4}, true);

    auto r = peerManager.load(localhost(cfg2.PEER_PORT));
    REQUIRE(r.second);
    REQUIRE(r.first.mNumFailures == 0);
}

TEST_CASE("peer is purged from database after few failures",
          "[overlay][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);
    auto record = [](size_t numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    SIMULATION_CREATE_NODE(Node1);

    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.push_back(vNode1NodeID);

    Config cfg1 = getTestConfig(1);
    Config cfg2 = getTestConfig(2);

    cfg1.PEER_AUTHENTICATION_TIMEOUT = 1;

    cfg2.MAX_INBOUND_PENDING_CONNECTIONS = 0;
    cfg2.MAX_OUTBOUND_PENDING_CONNECTIONS = 4; // to prevent changes in adjust()

    auto app1 = simulation->addNode(vNode1SecretKey, qSet, &cfg1);

    simulation->startAllNodes();

    auto& om = app1->getOverlayManager();
    auto& peerManager = om.getPeerManager();
    peerManager.store(localhost(cfg2.PEER_PORT), record(119), false);
    REQUIRE(peerManager.load(localhost(cfg2.PEER_PORT)).second);

    simulation->crankForAtLeast(std::chrono::seconds{5}, true);

    REQUIRE(!peerManager.load(localhost(cfg2.PEER_PORT)).second);
}

TEST_CASE("disconnected topology recovery", "[overlay][simulation]")
{
    auto initCfg = getTestConfig();
    auto cfgs = std::vector<Config>{};
    auto peers = std::vector<std::string>{};

    for (int i = 0; i < 7; ++i)
    {
        auto cfg = getTestConfig(i + 1);
        cfgs.push_back(cfg);
        peers.push_back("127.0.0.1:" + std::to_string(cfg.PEER_PORT));
    }

    auto doTest = [&](bool usePreferred) {
        auto simulation = Topologies::separate(
            7, 0.5, Simulation::OVER_LOOPBACK,
            sha256(initCfg.NETWORK_PASSPHRASE), 0, [&](int i) {
                if (i == 0)
                {
                    return initCfg;
                }
                auto cfg = cfgs[i - 1];
                cfg.TARGET_PEER_CONNECTIONS = 1;
                if (usePreferred)
                {
                    cfg.PREFERRED_PEERS = peers;
                }
                else
                {
                    cfg.KNOWN_PEERS = peers;
                }
                cfg.RUN_STANDALONE = false;
                return cfg;
            });
        auto nodeIDs = simulation->getNodeIDs();

        // Disconnected graph 0-1-2-3 and 4-5-6
        simulation->addPendingConnection(nodeIDs[0], nodeIDs[1]);
        simulation->addPendingConnection(nodeIDs[1], nodeIDs[2]);
        simulation->addPendingConnection(nodeIDs[2], nodeIDs[3]);
        simulation->addPendingConnection(nodeIDs[3], nodeIDs[0]);

        simulation->addPendingConnection(nodeIDs[6], nodeIDs[4]);
        simulation->addPendingConnection(nodeIDs[4], nodeIDs[5]);
        simulation->addPendingConnection(nodeIDs[5], nodeIDs[6]);

        simulation->startAllNodes();

        // Make sure connections are authenticated
        simulation->crankForAtLeast(std::chrono::seconds(1), false);
        auto nodes = simulation->getNodes();
        for (auto const& node : nodes)
        {
            REQUIRE(node->getOverlayManager().getAuthenticatedPeersCount() ==
                    2);
        }

        simulation->crankForAtLeast(
            std::chrono::seconds(
                Herder::CONSENSUS_STUCK_TIMEOUT_SECONDS.count() + 1),
            false);

        // Herder is not tracking (did not hear externalize from the network)
        REQUIRE(!nodes[4]->getHerder().isTracking());
        REQUIRE(!nodes[5]->getHerder().isTracking());
        REQUIRE(!nodes[6]->getHerder().isTracking());

        // LM is "synced" from the LCL perspective
        REQUIRE(nodes[4]->getLedgerManager().isSynced());
        REQUIRE(nodes[5]->getLedgerManager().isSynced());
        REQUIRE(nodes[6]->getLedgerManager().isSynced());

        // Crank long enough for overlay recovery to kick in
        simulation->crankForAtLeast(std::chrono::seconds(180), false);

        // If regular peers: Herder is now tracking due to reconnect
        // If preferred: Herder is still out of sync since no reconnects
        // happened
        REQUIRE(nodes[4]->getHerder().isTracking() == !usePreferred);
        REQUIRE(nodes[5]->getHerder().isTracking() == !usePreferred);
        REQUIRE(nodes[6]->getHerder().isTracking() == !usePreferred);

        // If regular peers: because we received a newer ledger, LM is now
        // "catching up" If preferred peers: no new ledgers heard, still
        // "synced"
        REQUIRE(nodes[4]->getLedgerManager().isSynced() == usePreferred);
        REQUIRE(nodes[5]->getLedgerManager().isSynced() == usePreferred);
        REQUIRE(nodes[6]->getLedgerManager().isSynced() == usePreferred);
    };

    SECTION("regular peers")
    {
        doTest(false);
    }
    SECTION("preferred peers")
    {
        doTest(true);
    }
}

TEST_CASE("overlay pull mode", "[overlay][pullmode]")
{
    VirtualClock clock;
    auto const numNodes = 3;
    std::vector<std::shared_ptr<Application>> apps;
    std::chrono::milliseconds const epsilon{1};

    for (auto i = 0; i < numNodes; i++)
    {
        Config cfg = getTestConfig(i);
        cfg.FLOOD_DEMAND_BACKOFF_DELAY_MS = std::chrono::milliseconds(200);
        cfg.FLOOD_DEMAND_PERIOD_MS = std::chrono::milliseconds(200);
        // Using a small tx set size such as 50 may lead to an unexpectedly
        // small advert/demand size limit.
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
        apps.push_back(createTestApplication(clock, cfg));
    }

    std::vector<std::shared_ptr<LoopbackPeerConnection>> connections;
    for (auto i = 0; i < numNodes; i++)
    {
        connections.push_back(std::make_shared<LoopbackPeerConnection>(
            *apps[i], *apps[(i + 1) % numNodes]));
    }
    testutil::crankFor(clock, std::chrono::seconds(5));
    for (auto& conn : connections)
    {
        REQUIRE(conn->getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn->getAcceptor()->isAuthenticatedForTesting());
    }

    auto createTxn = [](auto n) {
        StellarMessage txn;
        txn.type(TRANSACTION);
        Memo memo(MEMO_TEXT);
        memo.text() = "tx" + std::to_string(n);
        txn.transaction().v0().tx.memo = memo;

        return std::make_shared<StellarMessage>(txn);
    };

    auto createAdvert = [](auto txns) {
        StellarMessage adv;
        adv.type(FLOOD_ADVERT);
        for (auto const& txn : txns)
        {
            adv.floodAdvert().txHashes.push_back(xdrSha256(txn->transaction()));
        }
        return std::make_shared<StellarMessage>(adv);
    };

    // +-------------+------------+---------+
    // |             | Initiator  | Acceptor|
    // +-------------+------------+---------+
    // |Connection 0 |     0      |    1    |
    // |Connection 1 |     1      |    2    |
    // |Connection 2 |     2      |    0    |
    // +-------------+------------+---------+

    // `links[i][j]->sendMessage` is an easy way to send a message
    // from node `i` to node `j`.
    std::shared_ptr<LoopbackPeer> links[numNodes][numNodes];
    for (auto i = 0; i < numNodes; i++)
    {
        auto j = (i + 1) % 3;
        links[i][j] = connections[i]->getInitiator();
        links[j][i] = connections[i]->getAcceptor();
    }

    SECTION("ignore duplicated adverts")
    {
        auto tx = createTxn(0);
        auto adv =
            createAdvert(std::vector<std::shared_ptr<StellarMessage>>{tx});

        // Node 0 advertises tx 0 to Node 2
        links[0][2]->sendMessage(adv, false);
        links[0][2]->sendMessage(adv, false);
        links[0][2]->sendMessage(adv, false);

        // Give enough time to call `demand` multiple times
        testutil::crankFor(
            clock, 3 * apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS + epsilon);

        REQUIRE(getSentDemandCount(apps[2]) == 1);
        REQUIRE(getUnknownDemandCount(apps[0]) == 1);

        // 10 seconds is long enough for a few timeouts to fire
        // but not long enough for the pending demand record to drop.
        testutil::crankFor(clock, std::chrono::seconds(10));

        links[0][2]->sendMessage(adv, false);

        // Give enough time to call `demand` multiple times
        testutil::crankFor(
            clock, 3 * apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS + epsilon);

        REQUIRE(getSentDemandCount(apps[2]) == 1);
        REQUIRE(getUnknownDemandCount(apps[0]) == 1);
    }

    SECTION("do not advertise to peers that know about tx")
    {
        auto root = TestAccount::createRoot(*apps[0]);
        auto tx = root.tx({txtest::createAccount(
            txtest::getAccount("acc").getPublicKey(), 100)});
        auto adv =
            createAdvert(std::vector<std::shared_ptr<StellarMessage const>>{
                tx->toStellarMessage()});
        auto twoNodesRecvTx = [&]() {
            // Node0 and Node1 know about tx0 and will advertise it to Node2
            REQUIRE(apps[0]->getHerder().recvTransaction(tx, true).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            REQUIRE(apps[1]->getHerder().recvTransaction(tx, true).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        };

        SECTION("pull mode enabled on all")
        {
            twoNodesRecvTx();

            // Give enough time for Node2 to issue a demand and receive tx0
            testutil::crankFor(clock, std::chrono::seconds(1));

            REQUIRE(getSentDemandCount(apps[2]) == 1);
            // Either Node0 or Node1 fulfill the demand
            auto fulfilled = getFulfilledDemandCount(apps[0]) +
                             getFulfilledDemandCount(apps[1]);
            REQUIRE(fulfilled == 1);
            // After receiving a transaction, Node2 does not advertise it to
            // anyone because others already know about it
            REQUIRE(apps[2]
                        ->getMetrics()
                        .NewTimer({"overlay", "recv", "flood-advert"})
                        .count() == 2);
            REQUIRE(apps[2]
                        ->getMetrics()
                        .NewTimer({"overlay", "recv", "transaction"})
                        .count() == 1);
            REQUIRE(getAdvertisedHashCount(apps[2]) == 0);
        }
    }

    SECTION("sanity check - demand")
    {
        auto tx0 = createTxn(0);
        auto tx1 = createTxn(1);
        auto adv0 =
            createAdvert(std::vector<std::shared_ptr<StellarMessage>>{tx0});
        auto adv1 =
            createAdvert(std::vector<std::shared_ptr<StellarMessage>>{tx1});

        // Node 0 advertises tx 0 to Node 2
        links[0][2]->sendMessage(adv0, false);
        // Node 1 advertises tx 1 to Node 2
        links[1][2]->sendMessage(adv1, false);

        // Give enough time to:
        // 1) call `demand`, and
        // 2) send the demands out.
        testutil::crankFor(clock, apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS +
                                      epsilon);

        REQUIRE(getSentDemandCount(apps[2]) == 2);
        REQUIRE(getUnknownDemandCount(apps[0]) == 1);
        REQUIRE(getUnknownDemandCount(apps[1]) == 1);
    }

    SECTION("exact same advert from two peers")
    {
        std::vector<std::shared_ptr<StellarMessage>> txns;
        auto const numTxns = 5;
        txns.reserve(numTxns);
        for (auto i = 0; i < numTxns; i++)
        {
            txns.push_back(createTxn(i));
        }
        auto adv = createAdvert(txns);

        // Both Node 0 and Node 1 advertise {tx0, tx1, ..., tx5} to Node 2
        links[0][2]->sendMessage(adv, false);
        links[1][2]->sendMessage(adv, false);

        // Give enough time to:
        // 1) call `demand` exactly once, and
        // 2) send the demands out.
        testutil::crankFor(clock, apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS +
                                      epsilon);

        REQUIRE(getSentDemandCount(apps[2]) == 2);
        {
            // Node 2 is supposed to split the 5 demands evenly between Node 0
            // and Node 1 with no overlap.
            auto n0 = getUnknownDemandCount(apps[0]);
            auto n1 = getUnknownDemandCount(apps[1]);
            REQUIRE(std::min(n0, n1) == 2);
            REQUIRE(std::max(n0, n1) == 3);
            REQUIRE((n0 + n1) == 5);
        }

        // Wait long enough so the first round of demands expire and the second
        // round of demands get sent out.
        testutil::crankFor(
            clock, std::max(apps[2]->getConfig().FLOOD_DEMAND_BACKOFF_DELAY_MS,
                            apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS) +
                       epsilon);

        // Now both nodes should have gotten demands for all the 5 txn hashes.
        REQUIRE(getSentDemandCount(apps[2]) == 4);
        REQUIRE(getUnknownDemandCount(apps[0]) == 5);
        REQUIRE(getUnknownDemandCount(apps[1]) == 5);
    }

    SECTION("overlapping adverts")
    {
        auto tx0 = createTxn(0);
        auto tx1 = createTxn(1);
        auto tx2 = createTxn(2);
        auto tx3 = createTxn(3);
        auto adv0 = createAdvert(
            std::vector<std::shared_ptr<StellarMessage>>{tx0, tx1, tx3});
        auto adv1 = createAdvert(
            std::vector<std::shared_ptr<StellarMessage>>{tx0, tx2, tx3});

        // Node 0 advertises {tx0, tx1, tx3} to Node 2
        links[0][2]->sendMessage(adv0, false);
        // Node 1 advertises {tx0, tx2, tx3} to Node 2
        links[1][2]->sendMessage(adv1, false);

        // Give enough time to:
        // 1) call `demand`, and
        // 2) send the demands out.
        testutil::crankFor(clock, apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS +
                                      epsilon);

        REQUIRE(getSentDemandCount(apps[2]) == 2);

        {
            // Node 0 should get a demand for tx 1 and one of {tx 0, tx 3}.
            // Node 1 should get a demand for tx 2 and one of {tx 0, tx 3}.
            REQUIRE(getUnknownDemandCount(apps[0]) == 2);
            REQUIRE(getUnknownDemandCount(apps[1]) == 2);
        }

        // Wait long enough so the first round of demands expire and the second
        // round of demands get sent out.
        testutil::crankFor(clock,
                           apps[2]->getConfig().FLOOD_DEMAND_BACKOFF_DELAY_MS +
                               epsilon);

        // Node 0 should get a demand for the other member of {tx 0, tx 3}.
        // The same for Node 1.
        REQUIRE(getSentDemandCount(apps[2]) == 4);
        REQUIRE(getUnknownDemandCount(apps[0]) == 3);
        REQUIRE(getUnknownDemandCount(apps[1]) == 3);
    }

    SECTION("randomize peers")
    {
        auto peer0 = 0;
        auto peer1 = 0;
        auto const numRounds = 300;
        auto const numTxns = 5;
        for (auto i = 0; i < numRounds; i++)
        {
            std::vector<std::shared_ptr<StellarMessage>> txns;
            txns.reserve(numTxns);
            for (auto j = 0; j < numTxns; j++)
            {
                txns.push_back(createTxn(i * numTxns + j));
            }
            auto adv = createAdvert(txns);

            // Both Node 0 and Node 1 advertise {tx0, tx1, ..., tx5} to Node 2
            links[0][2]->sendMessage(adv, false);
            links[1][2]->sendMessage(adv, false);

            // Give enough time to:
            // 1) call `demand`, and
            // 2) send the demands out.
            testutil::crankFor(
                clock, apps[2]->getConfig().FLOOD_DEMAND_PERIOD_MS + epsilon);

            REQUIRE(getSentDemandCount(apps[2]) == i * 4 + 2);
            {
                // Node 2 should split the 5 txn hashes
                // evenly among Node 0 and Node 1.
                auto n0 = getUnknownDemandCount(apps[0]);
                auto n1 = getUnknownDemandCount(apps[1]);
                REQUIRE(std::max(n0, n1) == i * numTxns + 3);
                REQUIRE(std::min(n0, n1) == i * numTxns + 2);
                if (n0 < n1)
                {
                    peer1++;
                }
                else
                {
                    peer0++;
                }
            }

            // Wait long enough so the first round of demands expire and the
            // second round of demands get sent out.
            testutil::crankFor(
                clock,
                apps[2]->getConfig().FLOOD_DEMAND_BACKOFF_DELAY_MS + epsilon);
            REQUIRE(getUnknownDemandCount(apps[0]) == (i + 1) * numTxns);
            REQUIRE(getUnknownDemandCount(apps[1]) == (i + 1) * numTxns);
        }

        // In each of the 300 rounds, both peer0 and peer1 have
        // a 50% chance of getting the demand with 3 txns instead of 2.
        // Statistically speaking, this is the same as coin flips.
        // After 300 flips, the chance that we have more than 200 heads
        // is 0.000000401%.
        REQUIRE(std::max(peer0, peer1) <= numRounds * 2 / 3);
    }
    for (auto& app : apps)
    {
        testutil::shutdownWorkScheduler(*app);
    }
}

TEST_CASE("overlay pull mode loadgen", "[overlay][pullmode][acceptance]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID);

    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(vNode1NodeID);
    qSet.validators.push_back(vNode2NodeID);

    auto configs = std::vector<Config>{};
    auto const numAccounts = 5;

    for (auto i = 0; i < 2; i++)
    {
        auto cfg = getTestConfig(i + 1);
        cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = numAccounts * MAX_OPS_PER_TX;
        configs.push_back(cfg);
    }

    Application::pointer node1 =
        simulation->addNode(vNode1SecretKey, qSet, &configs[0]);
    Application::pointer node2 =
        simulation->addNode(vNode2SecretKey, qSet, &configs[1]);

    simulation->addPendingConnection(vNode1NodeID, vNode2NodeID);
    simulation->startAllNodes();

    simulation->crankUntil(
        [&] { return simulation->haveAllExternalized(2, 1); },
        3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto& loadGen = node1->getLoadGenerator();

    // Create 5 txns each creating one new account.
    // Set a really high tx rate so we create the txns right away.
    loadGen.generateLoad(GeneratedLoadConfig::createAccountsLoad(
        /* nAccounts */ numAccounts * MAX_OPS_PER_TX,
        /* txRate */ 1));

    // Let the network close multiple ledgers.
    // If the logic to advertise or demand incorrectly sends more than
    // they're supposed to (e.g., advertise the same txn twice),
    // then it'll likely happen within a few ledgers.
    simulation->crankUntil(
        [&] { return simulation->haveAllExternalized(5, 1); },
        10 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    // Node 1 advertised 5 txn hashes to each of Node 2 and Node 3.
    REQUIRE(getAdvertisedHashCount(node1) == numAccounts);
    REQUIRE(getAdvertisedHashCount(node2) == 0);

    REQUIRE(overlaytestutils::getSentDemandCount(node2) > 0);
    REQUIRE(overlaytestutils::getFulfilledDemandCount(node1) == numAccounts);

    // As this is a "happy path", there should be no unknown demands.
    REQUIRE(getUnknownDemandCount(node1) == 0);
    REQUIRE(getUnknownDemandCount(node2) == 0);
}

TEST_CASE("overlay pull mode with many peers",
          "[overlay][pullmode][acceptance]")
{
    VirtualClock clock;

    // Defined in src/overlay/OverlayManagerImpl.h.
    auto const maxRetry = 15;

    auto const numNodes = maxRetry + 5;
    std::vector<std::shared_ptr<Application>> apps;

    for (auto i = 0; i < numNodes; i++)
    {
        Config cfg = getTestConfig(i);
        apps.push_back(createTestApplication(clock, cfg));
    }

    std::vector<std::shared_ptr<LoopbackPeerConnection>> connections;
    // Every node is connected to node 0.
    for (auto i = 1; i < numNodes; i++)
    {
        connections.push_back(
            std::make_shared<LoopbackPeerConnection>(*apps[i], *apps[0]));
    }

    testutil::crankFor(clock, std::chrono::seconds(5));
    for (auto& conn : connections)
    {
        REQUIRE(conn->getInitiator()->isAuthenticatedForTesting());
        REQUIRE(conn->getAcceptor()->isAuthenticatedForTesting());
    }

    StellarMessage adv, emptyMsg;
    adv.type(FLOOD_ADVERT);
    // As we will never fulfill the demand in this test,
    // we won't even bother hashing an actual txn envelope.
    adv.floodAdvert().txHashes.push_back(xdrSha256(emptyMsg));
    for (auto& conn : connections)
    {
        // Everyone advertises to Node 0.
        conn->getInitiator()->sendMessage(
            std::make_shared<StellarMessage>(adv));
    }

    // Let it crank for 10 minutes.
    // If we're ever going to retry too many times,
    // it's likely that they'll happen in 10 minutes.
    testutil::crankFor(clock, std::chrono::minutes(10));

    REQUIRE(getSentDemandCount(apps[0]) == maxRetry);
}
}
