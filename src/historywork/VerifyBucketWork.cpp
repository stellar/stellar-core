// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyBucketWork.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
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

VerifyBucketWork::VerifyBucketWork(
    Application& app, std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::string const& bucketFile, uint256 const& hash, OnFailureCallback cb)
    : BasicWork(app, "verify-bucket-hash-" + bucketFile, BasicWork::RETRY_NEVER)
    , mBuckets(buckets)
    , mBucketFile(bucketFile)
    , mHash(hash)
    , mOnFailure(cb)
    , mVerifyBucketSuccess(app.getMetrics().NewMeter(
          {"history", "verify-bucket", "success"}, "event"))
    , mVerifyBucketFailure(app.getMetrics().NewMeter(
          {"history", "verify-bucket", "failure"}, "event"))
{
}

BasicWork::State
VerifyBucketWork::onRun()
{
    ZoneScoped;
    if (mDone)
    {
        if (mEc)
        {
            mVerifyBucketFailure.Mark();
            return State::WORK_FAILURE;
        }

        adoptBucket();
        mVerifyBucketSuccess.Mark();
        return State::WORK_SUCCESS;
    }

    spawnVerifier();
    return State::WORK_WAITING;
}

void
VerifyBucketWork::adoptBucket()
{
    ZoneScoped;
    assert(mDone);
    assert(!mEc);

    auto b = mApp.getBucketManager().adoptFileAsBucket(mBucketFile, mHash,
                                                       /*objectsPut=*/0,
                                                       /*bytesPut=*/0);
    mBuckets[binToHex(mHash)] = b;
}

void
VerifyBucketWork::spawnVerifier()
{
    std::string filename = mBucketFile;
    uint256 hash = mHash;
    Application& app = this->mApp;
    std::weak_ptr<VerifyBucketWork> weak(
        std::static_pointer_cast<VerifyBucketWork>(shared_from_this()));
    app.postOnBackgroundThread(
        [&app, filename, weak, hash]() {
            SHA256 hasher;
            asio::error_code ec;
            try
            {
                ZoneNamedN(verifyZone, "bucket verify", true);
                CLOG_INFO(History, "Verifying bucket {}", binToHex(hash));

                // ensure that the stream gets its own scope to avoid race with
                // main thread
                std::ifstream in(filename, std::ifstream::binary);
                if (!in)
                {
                    throw std::runtime_error(
                        fmt::format("Error opening file {}", filename));
                }
                in.exceptions(std::ios::badbit);
                char buf[4096];
                while (in)
                {
                    in.read(buf, sizeof(buf));
                    hasher.add(ByteSlice(buf, in.gcount()));
                }
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

void
VerifyBucketWork::onFailureRaise()
{
    if (mOnFailure)
    {
        mOnFailure();
    }
}
}
