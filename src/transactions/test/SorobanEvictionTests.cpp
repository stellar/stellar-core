// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/CovMark.h"
#include "test/test.h"
#include "test/Catch2.h"
#include "test/TxTests.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Logging.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("defer eviction due to concurrent TTL modify", "[tx][soroban][archival]")
{
    auto cfg = getTestConfig();
    SorobanTest test(cfg, true, [](SorobanNetworkConfig& cfg) {
        cfg.mStateArchivalSettings.minPersistentTTL =
            MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;
        cfg.mStateArchivalSettings.startingEvictionScanLevel = 1;
    });
    ContractStorageTestClient client(test);

    auto expirationLedger =
        test.getLCLSeq() +
        MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME;

    client.put("key", ContractDataDurability::PERSISTENT, 123);

    CLOG_INFO(Ledger, "Expiration ledger is {}", expirationLedger);

    while (test.getLCLSeq() < expirationLedger - 1)
    {
        CLOG_INFO(Ledger, "Closing ledger as LCL {} < Expiration ledger - 1 = {}", test.getLCLSeq(), expirationLedger - 1);
        closeLedger(test.getApp());
    }

    auto checklive = [&](ExpirationStatus expected) {
        auto status =
            client.getEntryExpirationStatus("key", ContractDataDurability::PERSISTENT);
        CLOG_INFO(Ledger, "Checking status at ledger {}, expected {}, found {}", test.getLCLSeq(), (int)expected,
                  (int)status);
        REQUIRE(status == expected);
    };

    checklive(ExpirationStatus::LIVE);
    closeLedger(test.getApp()); 
    checklive(ExpirationStatus::EXPIRED_IN_LIVE_STATE);

    COVMARK_CHECK_HIT_IN_CURR_SCOPE(EVICTION_TTL_MODIFIED_BETWEEN_DECISION_AND_EVICTION);

    // At present _this_ will trigger the restored-while-evicting logic; I guess
    // because eviction didn't quite get to the key in the previous ledger, even
    // though the entry _expired_ it wasn't evicted yet? Can we provoke that?
    client.restore("key", ContractDataDurability::PERSISTENT);
    checklive(ExpirationStatus::LIVE);

    while (test.getLCLSeq() < expirationLedger - 1)
    {
        CLOG_INFO(Ledger, "Closing ledger as LCL {} < Expiration ledger - 1 = {}", test.getLCLSeq(), expirationLedger - 1);
        closeLedger(test.getApp());
    }
    checklive(ExpirationStatus::LIVE);
    // Now do an extend _on_ the expiration ledger to ensure that eviction is deferred
    client.extend("key", ContractDataDurability::PERSISTENT,
                  MinimumSorobanNetworkConfig::MINIMUM_PERSISTENT_ENTRY_LIFETIME,
                  expirationLedger + 10);
}