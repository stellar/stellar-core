// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/LedgerCloseData.h"
#include "ledger/SyncingLedgerChain.h"
#include <lib/catch.hpp>

#include <memory>

using namespace stellar;

namespace
{

LedgerCloseData
makeLedgerCloseData(uint32_t ledgerSeq)
{
    auto txSet = std::make_shared<TxSetFrame>(sha256("a"), TransactionSet{});
    auto sv = StellarValue{};
    sv.txSetHash = txSet->getContentsHash();
    return LedgerCloseData{ledgerSeq, txSet, sv};
}
}

TEST_CASE("empty syncing ledger had not 'TOO_NEW'",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    REQUIRE(ledgerChain.size() == 0);
    REQUIRE(!ledgerChain.hadTooNew());
}

TEST_CASE("empty syncing ledger chain accepts anything",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    REQUIRE(ledgerChain.add(makeLedgerCloseData(1)) ==
            SyncingLedgerChainAddResult::CONTIGUOUS);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(!ledgerChain.hadTooNew());
    REQUIRE(ledgerChain.back().getLedgerSeq() == 1);
}

TEST_CASE("not empty syncing ledger chain accepts next ledger",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(1));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(2)) ==
            SyncingLedgerChainAddResult::CONTIGUOUS);
    REQUIRE(ledgerChain.size() == 2);
    REQUIRE(!ledgerChain.hadTooNew());
    REQUIRE(ledgerChain.back().getLedgerSeq() == 2);
}

TEST_CASE("not empty syncing ledger chain do not accept after-next ledger",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(1));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(3)) ==
            SyncingLedgerChainAddResult::TOO_NEW);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(ledgerChain.hadTooNew());
    REQUIRE(ledgerChain.back().getLedgerSeq() == 1);
}

TEST_CASE("not empty syncing ledger chain do not accept duplicate",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(1));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(1)) ==
            SyncingLedgerChainAddResult::TOO_OLD);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(!ledgerChain.hadTooNew());
    REQUIRE(ledgerChain.back().getLedgerSeq() == 1);
}

TEST_CASE("not empty syncing ledger chain do not accept previous ledger",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(2));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(1)) ==
            SyncingLedgerChainAddResult::TOO_OLD);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(!ledgerChain.hadTooNew());
    REQUIRE(ledgerChain.back().getLedgerSeq() == 2);
}

TEST_CASE("not empty syncing ledger chain accepts next ledger after failure",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(1));
    ledgerChain.add(makeLedgerCloseData(3));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(2)) ==
            SyncingLedgerChainAddResult::CONTIGUOUS);
    REQUIRE(ledgerChain.size() == 2);
    REQUIRE(ledgerChain.hadTooNew());
    REQUIRE(ledgerChain.back().getLedgerSeq() == 2);
}
