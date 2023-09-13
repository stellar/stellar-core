// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "main/Application.h"
#include "work/Work.h"
#include <filesystem>

namespace stellar
{

class CatchupWork;
class WorkSequence;

class ReplayDebugMetaWork : public Work
{
    // Target replay ledger
    uint32_t const mTargetLedger;

    // Debug meta files sorted in ascending order
    std::vector<std::filesystem::path> const mFiles;

    // Directory containing META_DEBUG_DIRNAME
    std::filesystem::path const mMetaDir;

    // File iterator to keep track of the next ledger batch to apply
    std::vector<std::filesystem::path>::const_iterator mNextToApply;

    BasicWork::State applyLastLedger();

    std::shared_ptr<WorkSequence> mCurrentWorkSequence;

  public:
    ReplayDebugMetaWork(Application& app, uint32_t targetLedger,
                        std::filesystem::path metaDir);
    virtual ~ReplayDebugMetaWork() = default;

  protected:
    BasicWork::State doWork() override;
    void doReset() override;
};
}
