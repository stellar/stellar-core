// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/ArchivalProofs.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

using namespace stellar;

TEST_CASE("creation proofs", "[archival][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS = true;
    auto app = createTestApplication(clock, cfg);

    SECTION("roundtrip")
    {
        xdr::xvector<ArchivalProof> proofs;
        auto entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_DATA}, 2);
        bool isAccount = true;
        for (auto& e : entries)
        {
            e.data.contractData().durability = PERSISTENT;
            e.data.contractData().key.type(SCV_SYMBOL);

            // We want two unique LedgerKeys, both the contract data keys should
            // both be "miss"
            e.data.contractData().key.sym() = "miss";
            if (isAccount)
            {
                e.data.contractData().contract.type(
                    SCAddressType::SC_ADDRESS_TYPE_ACCOUNT);
                isAccount = false;
            }
            else
            {
                e.data.contractData().contract.type(
                    SCAddressType::SC_ADDRESS_TYPE_CONTRACT);
            }
        }

        // Empty proof fails
        REQUIRE(checkCreationProofValidity(proofs));
        REQUIRE(!isCreatedKeyProven(*app, LedgerEntryKey(entries[0]), proofs));
        REQUIRE(addCreationProof(
            app->getConfig().ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS,
            LedgerEntryKey(entries[0]), proofs));
        REQUIRE(checkCreationProofValidity(proofs));
        REQUIRE(isCreatedKeyProven(*app, LedgerEntryKey(entries[0]), proofs));

        REQUIRE(proofs.size() == 1);
        REQUIRE(proofs.back().body.t() == NONEXISTENCE);
        REQUIRE(proofs.back().body.nonexistenceProof().keysToProve.size() == 1);

        // Proof with wrong key fails
        REQUIRE(!isCreatedKeyProven(*app, LedgerEntryKey(entries[1]), proofs));

        // Proofs work for multiple keys
        REQUIRE(addCreationProof(
            app->getConfig().ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS,
            LedgerEntryKey(entries[1]), proofs));
        REQUIRE(checkCreationProofValidity(proofs));
        REQUIRE(isCreatedKeyProven(*app, LedgerEntryKey(entries[1]), proofs));
        REQUIRE(isCreatedKeyProven(*app, LedgerEntryKey(entries[0]), proofs));

        REQUIRE(proofs.size() == 1);
        REQUIRE(proofs.back().body.t() == NONEXISTENCE);
        REQUIRE(proofs.back().body.nonexistenceProof().keysToProve.size() == 2);
    }

    SECTION("temp and code do not require creation proofs")
    {
        xdr::xvector<ArchivalProof> proofs;
        auto temp =
            LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
        temp.data.contractData().durability = TEMPORARY;
        auto code =
            LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_CODE);

        REQUIRE(checkCreationProofValidity(proofs));
        REQUIRE(isCreatedKeyProven(*app, LedgerEntryKey(temp), proofs));
        REQUIRE(addCreationProof(
            app->getConfig().ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS,
            LedgerEntryKey(temp), proofs));
        REQUIRE(checkCreationProofValidity(proofs));
        REQUIRE(proofs.size() == 0);

        REQUIRE(isCreatedKeyProven(*app, LedgerEntryKey(code), proofs));
        REQUIRE(addCreationProof(
            app->getConfig().ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS,
            LedgerEntryKey(code), proofs));
        REQUIRE(checkCreationProofValidity(proofs));
        REQUIRE(proofs.size() == 0);
    }
}

TEST_CASE("creation proof structure validity")
{
    xdr::xvector<ArchivalProof> proofs;
    auto temp = LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
    temp.data.contractData().durability = TEMPORARY;
    auto code = LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_CODE);
    auto classic = LedgerTestUtils::generateValidLedgerEntryOfType(TRUSTLINE);

    // Empty proof is valid
    REQUIRE(checkCreationProofValidity(proofs));

    SECTION("bad proof type")
    {
        proofs.emplace_back();
        proofs.back().body.t(EXISTENCE);
        REQUIRE(!checkCreationProofValidity(proofs));
    }

    SECTION("too many proofs structures")
    {
        proofs.emplace_back();
        proofs.back().body.t(NONEXISTENCE);
        proofs.emplace_back();
        proofs.back().body.t(NONEXISTENCE);
        REQUIRE(!checkCreationProofValidity(proofs));
    }

    SECTION("invalid keys")
    {
        proofs.emplace_back();
        proofs.back().body.t(NONEXISTENCE);

        SECTION("classic")
        {
            proofs.back().body.nonexistenceProof().keysToProve.emplace_back(
                LedgerEntryKey(classic));
            REQUIRE(!checkCreationProofValidity(proofs));
        }

        SECTION("temp")
        {
            proofs.back().body.nonexistenceProof().keysToProve.emplace_back(
                LedgerEntryKey(temp));
            REQUIRE(!checkCreationProofValidity(proofs));
        }

        SECTION("code")
        {
            proofs.back().body.nonexistenceProof().keysToProve.emplace_back(
                LedgerEntryKey(code));
            REQUIRE(!checkCreationProofValidity(proofs));
        }
    }

    SECTION("duplicate keys")
    {
        proofs.emplace_back();
        proofs.back().body.t(NONEXISTENCE);
        auto entry =
            LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
        entry.data.contractData().durability = PERSISTENT;
        proofs.back().body.nonexistenceProof().keysToProve.emplace_back(
            LedgerEntryKey(entry));
        REQUIRE(checkCreationProofValidity(proofs));
        proofs.back().body.nonexistenceProof().keysToProve.emplace_back(
            LedgerEntryKey(entry));
        REQUIRE(!checkCreationProofValidity(proofs));
    }
}

TEST_CASE("restoration proof structure validity")
{
    xdr::xvector<ArchivalProof> proofs;
    auto temp = LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
    temp.data.contractData().durability = TEMPORARY;
    auto classic = LedgerTestUtils::generateValidLedgerEntryOfType(TRUSTLINE);
    auto be = ColdArchiveBucketEntry();
    be.type(COLD_ARCHIVE_ARCHIVED_LEAF);

    // Empty proof is valid
    REQUIRE(checkRestorationProofValidity(proofs));

    SECTION("bad proof type")
    {
        proofs.emplace_back();
        proofs.back().body.t(NONEXISTENCE);
        REQUIRE(!checkRestorationProofValidity(proofs));
    }

    SECTION("too many proofs structures")
    {
        proofs.emplace_back();
        proofs.back().body.t(EXISTENCE);
        proofs.emplace_back();
        proofs.back().body.t(EXISTENCE);
        REQUIRE(!checkRestorationProofValidity(proofs));
    }

    SECTION("invalid keys")
    {
        proofs.emplace_back();
        proofs.back().body.t(EXISTENCE);

        SECTION("classic")
        {
            be.archivedLeaf().archivedEntry = classic;
            proofs.back().body.existenceProof().entriesToProve.emplace_back(be);
            REQUIRE(!checkRestorationProofValidity(proofs));
        }

        SECTION("temp")
        {
            be.archivedLeaf().archivedEntry = temp;
            proofs.back().body.existenceProof().entriesToProve.emplace_back(be);
            REQUIRE(!checkRestorationProofValidity(proofs));
        }

        SECTION("bad BucketEntry type")
        {
            be.type(COLD_ARCHIVE_DELETED_LEAF);
            proofs.back().body.existenceProof().entriesToProve.emplace_back(be);
            REQUIRE(!checkRestorationProofValidity(proofs));
        }
    }

    SECTION("duplicate keys")
    {
        proofs.emplace_back();
        proofs.back().body.t(EXISTENCE);
        auto entry =
            LedgerTestUtils::generateValidLedgerEntryOfType(CONTRACT_DATA);
        entry.data.contractData().durability = PERSISTENT;
        be.archivedLeaf().archivedEntry = entry;
        proofs.back().body.existenceProof().entriesToProve.emplace_back(be);
        REQUIRE(checkRestorationProofValidity(proofs));
        proofs.back().body.existenceProof().entriesToProve.emplace_back(be);
        REQUIRE(!checkRestorationProofValidity(proofs));
    }
}