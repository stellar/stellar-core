// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/LedgerCloseData.h"
#include "ledger/SyncingLedgerChain.h"
#include "lib/catch.hpp"

#include <memory>

using namespace stellar;

namespace
{

LedgerCloseData
makeLedgerCloseData(uint32_t ledgerSeq)
{
    auto txSet = std::make_shared<TxSetFrame>(sha256("a"));
    auto sv = StellarValue{};
    sv.txSetHash = txSet->getContentsHash();
    return LedgerCloseData{ledgerSeq, txSet, sv};
}

LedgerHeaderHistoryEntry
makeLedgerHistoryEntry(uint32_t nextLedgerSeq)
{
    auto result = LedgerHeaderHistoryEntry{};
    result.header.ledgerSeq = nextLedgerSeq - 1;
    result.hash = sha256("a");
    return result;
}

LedgerHeaderHistoryEntry
makeBadLedgerHistoryEntry(uint32_t nextLedgerSeq)
{
    auto result = LedgerHeaderHistoryEntry{};
    result.header.ledgerSeq = nextLedgerSeq - 1;
    result.hash = sha256("b");
    return result;
}
}

TEST_CASE("empty syncing ledger return UNKNOWN_RECOVERABLE",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    REQUIRE(ledgerChain.size() == 0);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(2)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
}

TEST_CASE("empty syncing ledger chain accepts anything",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    REQUIRE(ledgerChain.add(makeLedgerCloseData(20)) ==
            SyncingLedgerChainAddResult::CONTIGUOUS);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(ledgerChain.back().getLedgerSeq() == 20);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
}

TEST_CASE("not empty syncing ledger chain accepts next ledger",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(20));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(21)) ==
            SyncingLedgerChainAddResult::CONTIGUOUS);
    REQUIRE(ledgerChain.size() == 2);
    REQUIRE(ledgerChain.back().getLedgerSeq() == 21);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(22)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(22)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
}

TEST_CASE("not empty syncing ledger chain do not accept after-next ledger",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(20));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(22)) ==
            SyncingLedgerChainAddResult::TOO_NEW);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(ledgerChain.back().getLedgerSeq() == 20);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(22)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(22)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
}

TEST_CASE("not empty syncing ledger chain do not accept duplicate",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(20));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(20)) ==
            SyncingLedgerChainAddResult::TOO_OLD);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(ledgerChain.back().getLedgerSeq() == 20);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
}

TEST_CASE("not empty syncing ledger chain do not accept previous ledger",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(20));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(19)) ==
            SyncingLedgerChainAddResult::TOO_OLD);
    REQUIRE(ledgerChain.size() == 1);
    REQUIRE(ledgerChain.back().getLedgerSeq() == 20);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE);
}

TEST_CASE("not empty syncing ledger chain accepts next ledger after failure",
          "[ledger][ledgerchain]")
{
    auto ledgerChain = SyncingLedgerChain{};
    ledgerChain.add(makeLedgerCloseData(20));
    ledgerChain.add(makeLedgerCloseData(22));
    REQUIRE(ledgerChain.add(makeLedgerCloseData(21)) ==
            SyncingLedgerChainAddResult::CONTIGUOUS);
    REQUIRE(ledgerChain.size() == 2);
    REQUIRE(ledgerChain.back().getLedgerSeq() == 21);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(19)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(20)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_OK);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(21)) ==
            HistoryManager::VERIFY_HASH_BAD);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeLedgerHistoryEntry(22)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
    REQUIRE(ledgerChain.verifyCatchupCandidate(makeBadLedgerHistoryEntry(22)) ==
            HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE);
}
