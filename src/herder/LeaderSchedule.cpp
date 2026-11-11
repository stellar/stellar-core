// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/LeaderSchedule.h"
#include "crypto/SHA.h"
#include "ledger/LedgerHeaderUtils.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <utility>

namespace stellar
{

std::vector<NodeID>
computeLeaderSchedule(Hash const& seed, std::vector<NodeID> nodes)
{
    std::vector<std::pair<Hash, NodeID>> keyed;
    keyed.reserve(nodes.size());
    for (auto& node : nodes)
    {
        SHA256 hasher;
        hasher.add(seed);
        hasher.add(xdr::xdr_to_opaque(node));
        keyed.emplace_back(hasher.finish(), std::move(node));
    }
    std::sort(keyed.begin(), keyed.end());
    keyed.erase(std::unique(keyed.begin(), keyed.end()), keyed.end());

    std::vector<NodeID> res;
    res.reserve(keyed.size());
    for (auto& k : keyed)
    {
        res.emplace_back(std::move(k.second));
    }
    return res;
}

LeaderTermOutcome
leaderTermOutcome(uint32_t ledgerSeq, StellarValue const& value,
                  bool appliedTransactions)
{
    LeaderTermOutcome outcome;
    if ((static_cast<uint64_t>(ledgerSeq) + 1) % LEADER_TERM_LEDGERS == 0 ||
        value.ext.v() == STELLAR_VALUE_BASIC)
    {
        return outcome;
    }
    auto const& signer = getLcValueSignature(value).nodeID;
    if (isEmptyTxSetStellarValue(value) || !appliedTransactions)
    {
        outcome.mForfeited = signer;
    }
    else
    {
        outcome.mIncumbent = signer;
    }
    return outcome;
}

LeaderTermOutcome
leaderTermOutcome(LedgerHeader const& lcl, bool ledgersHaveResults)
{
    static Hash const emptyResults = xdrSha256(TransactionResultSet{});
    return leaderTermOutcome(lcl.ledgerSeq, lcl.scpValue,
                             !ledgersHaveResults ||
                                 lcl.txSetResultHash != emptyResults);
}

std::vector<NodeID>
computeLeaderSchedule(Hash const& seed, std::vector<NodeID> nodes,
                      LeaderTermOutcome const& outcome)
{
    auto schedule = computeLeaderSchedule(seed, std::move(nodes));
    // Both keep the seeded order of everyone else.
    if (outcome.mIncumbent)
    {
        auto it =
            std::find(schedule.begin(), schedule.end(), *outcome.mIncumbent);
        if (it != schedule.end())
        {
            std::rotate(schedule.begin(), it, it + 1);
        }
    }
    if (outcome.mForfeited)
    {
        auto it =
            std::find(schedule.begin(), schedule.end(), *outcome.mForfeited);
        if (it != schedule.end())
        {
            std::rotate(it, it + 1, schedule.end());
        }
    }
    return schedule;
}

std::vector<NodeID>
computeNextLeaderSchedule(LedgerHeader const& lcl, std::vector<NodeID> nodes,
                          bool ledgersHaveResults)
{
    return computeLeaderSchedule(lcl.previousLedgerHash, std::move(nodes),
                                 leaderTermOutcome(lcl, ledgersHaveResults));
}
}
