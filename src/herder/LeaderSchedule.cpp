// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/LeaderSchedule.h"
#include "crypto/SHA.h"
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
}
