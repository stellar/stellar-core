// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyBucketWork.h"
#include "bucket/LiveBucketIndex.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "history/HistoryArchive.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include <fmt/format.h>

#include <Tracy.hpp>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

#include <fstream>

namespace stellar
{

template <typename BucketT>
VerifyBucketWork<BucketT>::VerifyBucketWork(
    Application& app, std::string const& bucketFile, uint256 const& hash,
    std::unique_ptr<typename BucketT::IndexT const>& index,
    OnFailureCallback failureCb)
    : BasicWork(app, "verify-bucket-hash-" + bucketFile, BasicWork::RETRY_NEVER)
    , mBucketFile(bucketFile)
    , mHash(hash)
    , mIndex(index)
    , mOnFailure(failureCb)
{
}

template <typename BucketT>
BasicWork::State
VerifyBucketWork<BucketT>::onRun()
{
    ZoneScoped;
    if (mDone)
    {
        if (mEc)
        {
            return State::WORK_FAILURE;
        }
        return State::WORK_SUCCESS;
    }

    spawnVerifier();
    return State::WORK_WAITING;
}

template <typename BucketT>
void
VerifyBucketWork<BucketT>::spawnVerifier()
{
    std::string filename = mBucketFile;
    if (auto size = fs::size(filename);
        size > HistoryArchiveState::MAX_HISTORY_ARCHIVE_BUCKET_SIZE)
    {
        CLOG_WARNING(History,
                     "Failed verification: Bucket size ({}) is greater than "
                     "the maximum allowed size ({}). Expected hash: {}",
                     size, HistoryArchiveState::MAX_HISTORY_ARCHIVE_BUCKET_SIZE,
                     binToHex(mHash));

        mEc = std::make_error_code(std::errc::io_error);
        mDone = true;
        return;
    }

    uint256 hash = mHash;
    Application& app = this->mApp;
    std::weak_ptr<VerifyBucketWork> weak(
        std::static_pointer_cast<VerifyBucketWork>(shared_from_this()));
    app.postOnBackgroundThread(
        [&app, filename, weak, hash, &index = mIndex]() {
            SHA256 hasher;
            asio::error_code ec;

            // No point in verifying buckets if things are shutting down
            auto self = weak.lock();
            if (!self || self->isAborting())
            {
                return;
            }

            try
            {
                ZoneNamedN(verifyZone, "bucket verify", true);
                CLOG_INFO(History, "Verifying and indexing bucket {}",
                          binToHex(hash));

                index =
                    createIndex<BucketT>(app.getBucketManager(), filename, hash,
                                         app.getWorkerIOContext(), &hasher);
                releaseAssertOrThrow(index);

                uint256 vHash = hasher.finish();
                if (vHash == hash)
                {
                    CLOG_DEBUG(History, "Verified hash ({}) for {}",
                               hexAbbrev(hash), filename);
                }
                else
                {
                    CLOG_WARNING(History, "FAILED verifying hash for {}",
                                 filename);
                    CLOG_WARNING(History, "expected hash: {}", binToHex(hash));
                    CLOG_WARNING(History, "computed hash: {}", binToHex(vHash));
                    CLOG_WARNING(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
                    ec = std::make_error_code(std::errc::io_error);
                }
            }
            catch (std::exception const& e)
            {
                CLOG_WARNING(History, "Failed verification : {}", e.what());
                ec = std::make_error_code(std::errc::io_error);
            }

            // Not ideal, but needed to prevent race conditions with
            // main thread, since BasicWork's state is not thread-safe. This is
            // a temporary workaround, as a cleaner solution is needed.
            app.postOnMainThread(
                [weak, ec]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        self->mEc = ec;
                        self->mDone = true;
                        self->wakeUp();
                    }
                },
                "VerifyBucket: finish");
        },
        "VerifyBucket: start in background");
}

template <typename BucketT>
void
VerifyBucketWork<BucketT>::onFailureRaise()
{
    if (mOnFailure)
    {
        mOnFailure();
    }
}

template class VerifyBucketWork<LiveBucket>;
template class VerifyBucketWork<HotArchiveBucket>;
}
