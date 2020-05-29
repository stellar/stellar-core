// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PublishWork.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "history/StateSnapshot.h"
#include "main/Application.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

PublishWork::PublishWork(Application& app,
                         std::shared_ptr<StateSnapshot> snapshot,
                         std::vector<std::shared_ptr<BasicWork>> seq,
                         std::vector<std::string> const& bucketHashes)
    : WorkSequence(
          app,
          fmt::format("publish-{:08x}", snapshot->mLocalState.currentLedger),
          seq, BasicWork::RETRY_NEVER)
    , mSnapshot(snapshot)
    , mOriginalBuckets(bucketHashes)
{
}

void
PublishWork::onFailureRaise()
{
    ZoneScoped;
    // use mOriginalBuckets as mSnapshot->mLocalState.allBuckets() could change
    // in meantime
    mApp.getHistoryManager().historyPublished(
        mSnapshot->mLocalState.currentLedger, mOriginalBuckets, false);
}

void
PublishWork::onSuccess()
{
    ZoneScoped;
    // use mOriginalBuckets as mSnapshot->mLocalState.allBuckets() could change
    // in meantime
    mApp.getHistoryManager().historyPublished(
        mSnapshot->mLocalState.currentLedger, mOriginalBuckets, true);
}
}
