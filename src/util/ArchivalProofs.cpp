// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/ArchivalProofs.h"
#include "bucket/BucketListSnapshot.h"
#include "bucket/BucketManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/GlobalChecks.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include <memory>
#include <optional>
#include <stdexcept>

namespace stellar
{

bool
checkCreationProofValidity(xdr::xvector<ArchivalProof> const& proofs)
{
    if (proofs.size() > 1)
    {
        return false;
    }
    else if (proofs.empty())
    {
        return true;
    }

    auto const& proof = proofs[0];
    if (proof.body.t() != NONEXISTENCE)
    {
        return false;
    }

    auto numProvenKeys = proof.body.nonexistenceProof().keysToProve.size();
    if (numProvenKeys == 0)
    {
        return false;
    }

    // Require unique keys
    UnorderedSet<LedgerKey> keys(numProvenKeys);
    for (auto const& key : proof.body.nonexistenceProof().keysToProve)
    {
        if (key.type() != CONTRACT_DATA ||
            key.contractData().durability != PERSISTENT)
        {
            return false;
        }

        if (!keys.insert(key).second)
        {
            return false;
        }
    }

    return true;
}

bool
isCreatedKeyProven(Application& app, LedgerKey const& lk,
                   xdr::xvector<ArchivalProof> const& proofs)
{
    // Only persistent contract data entries need creation proofs
    if (lk.type() == CONTRACT_DATA &&
        lk.contractData().durability == PERSISTENT)
    {
#ifdef BUILD_TESTS
        if (app.getConfig().ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS)
        {
            // Artificially require proofs for "miss" keys
            if (lk.contractData().key.type() == SCV_SYMBOL &&
                lk.contractData().key.sym() == "miss")
            {
                if (proofs.size() != 1)
                {
                    return false;
                }

                releaseAssert(proofs.size() == 1);
                releaseAssert(proofs[0].body.t() == NONEXISTENCE);
                for (auto const& key :
                     proofs[0].body.nonexistenceProof().keysToProve)
                {
                    if (key == lk)
                    {
                        return true;
                    }
                }

                return false;
            }
        }
    }
#endif

    return true;
}

bool
addCreationProof(bool simulateBloomMiss, LedgerKey const& lk,
                 xdr::xvector<ArchivalProof>& proofs)
{
#ifdef BUILD_TESTS
    if (simulateBloomMiss)
    {
        // Only persistent contract data entries need creation proofs
        if (lk.type() != CONTRACT_DATA ||
            lk.contractData().durability != PERSISTENT)
        {
            return true;
        }

        for (auto& proof : proofs)
        {
            if (proof.body.t() == NONEXISTENCE)
            {
                for (auto const& key :
                     proof.body.nonexistenceProof().keysToProve)
                {
                    if (key == lk)
                    {
                        // Proof already exists
                        return true;
                    }
                }

                proof.body.nonexistenceProof().keysToProve.push_back(lk);
                return true;
            }
        }

        proofs.emplace_back();
        auto& nonexistenceProof = proofs.back();
        nonexistenceProof.body.t(NONEXISTENCE);
        nonexistenceProof.body.nonexistenceProof().keysToProve.push_back(lk);
    }
#endif

    return true;
}

bool
checkRestorationProofValidity(xdr::xvector<ArchivalProof> const& proofs)
{
    if (proofs.size() > 1)
    {
        return false;
    }
    else if (proofs.empty())
    {
        return true;
    }

    auto const& proof = proofs[0];
    if (proof.body.t() != EXISTENCE)
    {
        return false;
    }

    auto numProvenEntries = proof.body.existenceProof().entriesToProve.size();
    if (numProvenEntries == 0)
    {
        return false;
    }

    // Require unique keys
    UnorderedSet<LedgerKey> keys(numProvenEntries);
    for (auto const& be : proof.body.existenceProof().entriesToProve)
    {
        // All entries proven must be Archived BucketEntries
        if (be.type() != COLD_ARCHIVE_ARCHIVED_LEAF)
        {
            return false;
        }

        auto key = LedgerEntryKey(be.archivedLeaf().archivedEntry);
        if (!isPersistentEntry(key))
        {
            return false;
        }

        if (!keys.insert(key).second)
        {
            return false;
        }
    }

    return true;
}

std::shared_ptr<LedgerEntry>
getRestoredEntryFromProof(Application& app, LedgerKey const& lk,
                          xdr::xvector<ArchivalProof> const& proofs)
{
    releaseAssertOrThrow(isPersistentEntry(lk));
    auto hotArchive =
        app.getBucketManager().getSearchableHotArchiveBucketListSnapshot();
    auto entry = hotArchive->load(lk);
#ifdef BUILD_TESTS
    if (app.getConfig().REQUIRE_PROOFS_FOR_ALL_EVICTED_ENTRIES)
    {
        if (proofs.size() != 1)
        {
            return nullptr;
        }

        // Should have been checked already by checkRestorationProofValidity
        releaseAssertOrThrow(proofs[0].body.t() == EXISTENCE);
        for (auto const& be : proofs[0].body.existenceProof().entriesToProve)
        {
            if (LedgerEntryKey(be.archivedLeaf().archivedEntry) == lk)
            {
                if (entry &&
                    entry->archivedEntry() == be.archivedLeaf().archivedEntry)
                {
                    return std::make_shared<LedgerEntry>(
                        entry->archivedEntry());
                }

                return nullptr;
            }
        }

        return nullptr;
    }
#endif

    if (entry &&
        entry->type() == HotArchiveBucketEntryType::HOT_ARCHIVE_ARCHIVED)
    {
        return std::make_shared<LedgerEntry>(entry->archivedEntry());
    }

    return nullptr;
}

bool
addRestorationProof(
    std::shared_ptr<SearchableHotArchiveBucketListSnapshot> hotArchive,
    LedgerKey const& lk, xdr::xvector<ArchivalProof>& proofs,
    std::optional<uint32_t> ledgerSeq)
{
#ifdef BUILD_TESTS
    // For now only support proof generation for testing
    releaseAssertOrThrow(isPersistentEntry(lk));

    std::shared_ptr<HotArchiveBucketEntry> entry = nullptr;
    if (ledgerSeq)
    {
        auto entryOp = hotArchive->loadKeysFromLedger({lk}, *ledgerSeq);
        if (!entryOp)
        {
            throw std::invalid_argument("LedgerSeq not found");
        }

        entry =
            std::make_shared<HotArchiveBucketEntry>(std::move(entryOp->at(0)));
    }
    else
    {
        entry = hotArchive->load(lk);
    }

    if (!entry ||
        entry->type() != HotArchiveBucketEntryType::HOT_ARCHIVE_ARCHIVED)
    {
        return false;
    }

    ColdArchiveBucketEntry be;
    be.type(COLD_ARCHIVE_ARCHIVED_LEAF);
    be.archivedLeaf().archivedEntry = entry->archivedEntry();

    for (auto& proof : proofs)
    {
        if (proof.body.t() == EXISTENCE)
        {
            for (auto const& be : proof.body.existenceProof().entriesToProve)
            {
                if (LedgerEntryKey(be.archivedLeaf().archivedEntry) == lk)
                {
                    // Proof already exists
                    return true;
                }
            }

            proof.body.existenceProof().entriesToProve.push_back(be);
            return true;
        }
    }

    proofs.emplace_back();
    auto& existenceProof = proofs.back();
    existenceProof.body.t(EXISTENCE);
    existenceProof.body.existenceProof().entriesToProve.push_back(be);

#endif

    return true;
}
}