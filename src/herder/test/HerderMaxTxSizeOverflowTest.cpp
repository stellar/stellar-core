// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "test/Catch2.h"
#include <limits>

using namespace stellar;

TEST_CASE("max transaction size overflow prevention", "[herder][overflow]")
{
    VirtualClock clock;
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    Application::pointer app = createTestApplication(clock, cfg);

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    SECTION("overflow in start() method")
    {
        // First, modify the network configuration to have maximum transaction size
        modifySorobanNetworkConfig(*app, [&](SorobanNetworkConfig& sorobanCfg) {
            // Set to maximum value that would cause overflow
            sorobanCfg.mTxMaxSizeBytes = std::numeric_limits<uint32_t>::max();
        });

        // Get the flow control buffer size
        uint32_t flowControlBuffer = herder.getFlowControlExtraBuffer();
        uint32_t classicTxSize = herder.getMaxClassicTxSize();
        
        CLOG_DEBUG(Herder, "Flow control buffer: {}", flowControlBuffer);
        CLOG_DEBUG(Herder, "Classic tx size: {}", classicTxSize);
        CLOG_DEBUG(Herder, "Max uint32: {}", std::numeric_limits<uint32_t>::max());

        // Call start() which should handle the overflow properly
        herder.start();

        // The max tx size should not be a small value due to overflow
        uint32_t maxTxSize = herder.getMaxTxSize();
        
        CLOG_DEBUG(Herder, "Computed max tx size: {}", maxTxSize);
        
        // After the overflow fix, mMaxTxSize should be at least the classic tx size
        REQUIRE(maxTxSize >= classicTxSize);
        
        // It should not be a tiny value that indicates overflow occurred
        REQUIRE(maxTxSize > flowControlBuffer);
    }

    SECTION("overflow in maybeHandleUpgrade() method")
    {
        // First start normally
        herder.start();
        uint32_t initialMaxTxSize = herder.getMaxTxSize();
        
        // Now modify the network configuration to have maximum transaction size
        modifySorobanNetworkConfig(*app, [&](SorobanNetworkConfig& sorobanCfg) {
            // Set to maximum value that would cause overflow
            sorobanCfg.mTxMaxSizeBytes = std::numeric_limits<uint32_t>::max();
        });

        uint32_t flowControlBuffer = herder.getFlowControlExtraBuffer();
        uint32_t classicTxSize = herder.getMaxClassicTxSize();

        // Call maybeHandleUpgrade() which should handle the overflow properly
        herder.maybeHandleUpgrade();

        uint32_t newMaxTxSize = herder.getMaxTxSize();
        
        CLOG_DEBUG(Herder, "Initial max tx size: {}", initialMaxTxSize);
        CLOG_DEBUG(Herder, "New max tx size after upgrade: {}", newMaxTxSize);
        
        // After the overflow fix, mMaxTxSize should be at least the classic tx size
        REQUIRE(newMaxTxSize >= classicTxSize);
        
        // It should not be a tiny value that indicates overflow occurred
        REQUIRE(newMaxTxSize > flowControlBuffer);
    }
}

TEST_CASE("max transaction size overflow demonstration", "[herder][overflow][!shouldfail]")
{
    // This test demonstrates the overflow issue before the fix
    uint32_t maxUint32 = std::numeric_limits<uint32_t>::max();
    uint32_t flowControlBuffer = 2000; // FLOW_CONTROL_BYTES_EXTRA_BUFFER

    // This will overflow to a small value
    uint32_t overflowResult = maxUint32 + flowControlBuffer;
    
    CLOG_DEBUG(Herder, "Max uint32: {}", maxUint32);
    CLOG_DEBUG(Herder, "Flow control buffer: {}", flowControlBuffer);
    CLOG_DEBUG(Herder, "Overflow result: {}", overflowResult);
    
    // This demonstrates the overflow - the result wraps around to 1999
    REQUIRE(overflowResult == 1999);
    REQUIRE(overflowResult < flowControlBuffer);
    REQUIRE(overflowResult < 100 * 1024); // Classic tx size is 100KB
}