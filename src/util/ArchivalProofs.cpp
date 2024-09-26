// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/ArchivalProofs.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

namespace stellar
{
bool
checkCreationProof(Application& app, LedgerKey const& lk,
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

                auto const& proof = proofs[0];
                if (proof.body.t() != NONEXISTENCE)
                {
                    return false;
                }

                for (auto const& key :
                     proof.body.nonexistenceProof().keysToProve)
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
addCreationProof(Application& app, LedgerKey const& lk,
                 xdr::xvector<ArchivalProof>& proofs)
{
#ifdef BUILD_TESTS
    // For now only support proof generation for testing
    releaseAssertOrThrow(
        app.getConfig().ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS);

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
            for (auto const& key : proof.body.nonexistenceProof().keysToProve)
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
#endif

    return true;
}
}