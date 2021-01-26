// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;
using namespace stellar::InvariantTestUtils;

TEST_CASE("Trigger validity check for each entry type",
          "[invariant][ledgerentryisvalid]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LedgerEntryIsValid"};

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    LedgerEntry le;
    SECTION("account")
    {
        le.data.type(ACCOUNT);
        le.data.account() = LedgerTestUtils::generateValidAccountEntry(5);
        le.data.account().flags = MASK_ACCOUNT_FLAGS_V16 + 1;
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
    SECTION("trustline")
    {
        le.data.type(TRUSTLINE);
        le.data.trustLine() = LedgerTestUtils::generateValidTrustLineEntry(5);
        le.data.trustLine().flags = MASK_TRUSTLINE_FLAGS_V16 + 1;
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
    SECTION("offer")
    {
        le.data.type(OFFER);
        le.data.offer() = LedgerTestUtils::generateValidOfferEntry(5);
        le.data.offer().flags = MASK_OFFERENTRY_FLAGS + 1;
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
    SECTION("data")
    {
        le.data.type(DATA);
        le.data.data() = LedgerTestUtils::generateValidDataEntry(5);
        le.data.data().dataName.clear();
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
    SECTION("claimable balance")
    {
        le.data.type(CLAIMABLE_BALANCE);
        le.data.claimableBalance() =
            LedgerTestUtils::generateValidClaimableBalanceEntry(5);
        le.data.claimableBalance().claimants.clear();
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
}

TEST_CASE("Modify ClaimableBalanceEntry",
          "[invariant][ledgerentryisvalid][claimablebalance]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LedgerEntryIsValid"};

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto accountID = getAccount("acc").getPublicKey();

    LedgerEntry le;
    le.data.type(CLAIMABLE_BALANCE);
    le.data.claimableBalance() =
        LedgerTestUtils::generateValidClaimableBalanceEntry(5);
    prepareLedgerEntryExtensionV1(le).sponsoringID.activate() = accountID;

    ClaimPredicate pred;
    pred.type(CLAIM_PREDICATE_UNCONDITIONAL);

    Claimant c;
    c.v0().destination = accountID;
    c.v0().predicate = pred;

    le.data.claimableBalance().claimants = {c};
    auto leCopy = le;

    REQUIRE(store(*app, makeUpdateList({le}, nullptr)));

    SECTION("claimable balance is modified")
    {
        leCopy.data.claimableBalance().amount++;
        REQUIRE(!store(*app, makeUpdateList({leCopy}, {le})));
    }

    SECTION("claimable balance is not modified")
    {
        REQUIRE(store(*app, makeUpdateList({leCopy}, {le})));
    }
}