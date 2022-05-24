// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AssumeStateWork.h"
#include "bucket/BucketManager.h"
#include "catchup/IndexBucketsWork.h"
#include "work/WorkSequence.h"
#include "work/WorkWithCallback.h"

namespace stellar
{
AssumeStateWork::AssumeStateWork(Application& app,
                                 HistoryArchiveState const& has,
                                 uint32_t maxProtocolVersion)
    : Work(app, "assume-state", BasicWork::RETRY_NEVER)
    , mHas(has)
    , mMaxProtocolVersion(maxProtocolVersion)
{
}

BasicWork::State
AssumeStateWork::doWork()
{
    if (!mWorkSpawned)
    {
        std::vector<std::shared_ptr<BasicWork>> seq;

        auto assumeBLStateCB = [&has = mHas](Application& app) {
            app.getBucketManager().assumeState(has);
            return true;
        };
        auto assumeBLStateWork = std::make_shared<WorkWithCallback>(
            mApp, "assume-bl-state", assumeBLStateCB);
        seq.push_back(assumeBLStateWork);

        if (mApp.getConfig().shouldIndex())
        {
            seq.push_back(std::make_shared<IndexBucketsWork>(mApp));
        }

        auto restartMergesCB = [&has = mHas,
                                maxProtocolVersion =
                                    mMaxProtocolVersion](Application& app) {
            app.getBucketManager().restartMerges(has, maxProtocolVersion);
            return true;
        };
        auto restartMergesWork = std::make_shared<WorkWithCallback>(
            mApp, "restart-merges", restartMergesCB);
        seq.push_back(restartMergesWork);

        addWork<WorkSequence>("assume-state-seq", seq, RETRY_NEVER);

        mWorkSpawned = true;
        return State::WORK_RUNNING;
    }

    return checkChildrenStatus();
}

void
AssumeStateWork::doReset()
{
    mWorkSpawned = false;
}
}