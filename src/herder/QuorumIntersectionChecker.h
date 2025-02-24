#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumTracker.h"
#include "rust/RustBridge.h"
#include "util/TmpDir.h"
#include <atomic>
#include <memory>
#include <optional>

namespace stellar
{

class Config;
class Application;
class TmpDir;

struct QuorumMapIntersectionState
{
    uint32_t mLastCheckLedger{0};
    uint32_t mLastGoodLedger{0};
    size_t mNumNodes{0};
    Hash mLastCheckQuorumMapHash{};
    Hash mCheckingQuorumMapHash{};
    bool mRecalculating{false};

    // for v1 (QuorumIntersectionChecker)
    std::atomic<bool> mInterruptFlag{false};

    std::unique_ptr<TmpDir> mTmpDir;

    QuorumCheckerStatus mStatus{QuorumCheckerStatus::UNKNOWN};
    QuorumCheckerResource mResourceUsage{0, 0};
    std::pair<std::vector<PublicKey>, std::vector<PublicKey>> mPotentialSplit{};
    std::set<std::set<PublicKey>> mIntersectionCriticalNodes{};

    bool
    hasAnyResults() const
    {
        return mLastGoodLedger != 0;
    }

    bool
    enjoysQuorunIntersection() const
    {
        return mLastCheckLedger == mLastGoodLedger;
    }

    QuorumMapIntersectionState(TmpDir&& tmpDir)
        : mTmpDir(std::make_unique<TmpDir>(std::move(tmpDir)))
    {
    }
};

class QuorumIntersectionChecker
{
  public:
    using QuorumSetMap =
        stellar::UnorderedMap<stellar::NodeID, stellar::SCPQuorumSetPtr>;

    using PotentialSplit =
        std::pair<std::vector<PublicKey>, std::vector<PublicKey>>;
    // for v1 qic
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
        QuorumSetMap const& qmap, std::optional<Config> const& cfg,
        std::function<bool(QuorumSetMap const&,
                           std::optional<stellar::Config> const&)> const&
            networkEnjoysQuorumIntersectionCB);

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
