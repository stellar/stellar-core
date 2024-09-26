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
        REQUIRE(!checkCreationProof(*app, LedgerEntryKey(entries[0]), proofs));
        REQUIRE(addCreationProof(*app, LedgerEntryKey(entries[0]), proofs));
        REQUIRE(checkCreationProof(*app, LedgerEntryKey(entries[0]), proofs));

        REQUIRE(proofs.size() == 1);
        REQUIRE(proofs.back().body.t() == NONEXISTENCE);
        REQUIRE(proofs.back().body.nonexistenceProof().keysToProve.size() == 1);

        // Proof with wrong key fails
        REQUIRE(!checkCreationProof(*app, LedgerEntryKey(entries[1]), proofs));

        // Proofs work for multiple keys
        REQUIRE(addCreationProof(*app, LedgerEntryKey(entries[1]), proofs));
        REQUIRE(checkCreationProof(*app, LedgerEntryKey(entries[1]), proofs));
        REQUIRE(checkCreationProof(*app, LedgerEntryKey(entries[0]), proofs));

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

        REQUIRE(checkCreationProof(*app, LedgerEntryKey(temp), proofs));
        REQUIRE(addCreationProof(*app, LedgerEntryKey(temp), proofs));
        REQUIRE(proofs.size() == 0);

        REQUIRE(checkCreationProof(*app, LedgerEntryKey(code), proofs));
        REQUIRE(addCreationProof(*app, LedgerEntryKey(code), proofs));
        REQUIRE(proofs.size() == 0);
    }
}