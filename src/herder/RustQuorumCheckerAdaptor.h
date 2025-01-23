#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/QuorumIntersectionChecker.h"
#include "herder/QuorumTracker.h"
#include "main/Config.h"
#include "rust/RustBridge.h"
#include <optional>

namespace stellar
{

// static adaptor class over rust QuorumChecker that exposes its functionalities
class RustQuorumCheckerAdaptor
{

  public:
    static bool networkEnjoysQuorumIntersection(
        QuorumTracker::QuorumMap const& qmap,
        std::optional<stellar::Config> const& cfg,
        rust_bridge::quorum_checker::Interrupt const& interrupt,
        QuorumIntersectionChecker::PotentialSplit& potentialSplit);

    static bool networkEnjoysQuorumIntersection(
        QuorumIntersectionChecker::QuorumSetMap const& qmap,
        std::optional<stellar::Config> const& cfg,
        rust_bridge::quorum_checker::Interrupt const& interrupt,
        QuorumIntersectionChecker::PotentialSplit& potentialSplit);

    static std::atomic<uint32_t>
    getSuccessfulCallCount()
    {
        return mSuccessfulCallCount.load();
    }

    static std::atomic<uint32_t>
    getFailedCallCount()
    {
        return mFailedCallCount.load();
    }

    static std::atomic<uint32_t>
    getInterruptedCallCount()
    {
        return mInterruptedCallCount.load();
    }

    static std::atomic<uint32_t>
    getPotentialSplitCount()
    {
        return mPotentialSplitCount.load();
    }

  private:
    static std::atomic<uint32_t> mSuccessfulCallCount;
    static std::atomic<uint32_t> mFailedCallCount;
    static std::atomic<uint32_t> mInterruptedCallCount;
    static std::atomic<uint32_t> mPotentialSplitCount;

}; // class RustQuorumCheckerAdaptor {

class RustQuorumCheckerError : public std::runtime_error
{
  public:
    RustQuorumCheckerError(std::string const& msg) : std::runtime_error(msg)
    {
    }
};

} // namespace stellar {
