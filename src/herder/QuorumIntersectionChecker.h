#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumTracker.h"
#include <atomic>
#include <memory>
#include <optional>

namespace stellar
{

class Config;

class QuorumIntersectionChecker
{
  public:
    using QuorumSetMap =
        stellar::UnorderedMap<stellar::NodeID, stellar::SCPQuorumSetPtr>;

    static std::shared_ptr<QuorumIntersectionChecker>
    create(QuorumTracker::QuorumMap const& qmap,
           std::optional<stellar::Config> const& cfg,
           std::atomic<bool>& interruptFlag,
           stellar_default_random_engine::result_type seed, bool quiet = false);

    static std::shared_ptr<QuorumIntersectionChecker>
    create(QuorumSetMap const& qmap, std::optional<stellar::Config> const& cfg,
           std::atomic<bool>& interruptFlag,
           stellar_default_random_engine::result_type seed, bool quiet = false);

    static std::set<std::set<NodeID>> getIntersectionCriticalGroups(
        QuorumTracker::QuorumMap const& qmap,
        std::optional<stellar::Config> const& cfg,
        std::atomic<bool>& interruptFlag,
        stellar_default_random_engine::result_type seed);

    static std::set<std::set<NodeID>> getIntersectionCriticalGroups(
        QuorumSetMap const& qmap, std::optional<stellar::Config> const& cfg,
        std::atomic<bool>& interruptFlag,
        stellar_default_random_engine::result_type seed);

    virtual ~QuorumIntersectionChecker(){};
    virtual bool networkEnjoysQuorumIntersection() const = 0;
    virtual size_t getMaxQuorumsFound() const = 0;
    virtual std::pair<std::vector<NodeID>, std::vector<NodeID>>
    getPotentialSplit() const = 0;

    // If any thread sets the atomic interruptFlag passed into any of the above
    // methods, any calculation-in-progress will throw InterruptedException and
    // unwind.
    struct InterruptedException
    {
    };
};
}
