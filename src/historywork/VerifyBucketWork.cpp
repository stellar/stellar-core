// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyBucketWork.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

#include <fstream>

namespace stellar
{
VerifyBucketWork::VerifyBucketWork(
    Application& app, std::function<void()> callback,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::string const& bucketFile, uint256 const& hash)
    : Work(app, callback, "verify-bucket-hash-" + bucketFile, RETRY_NEVER)
    , mBuckets(buckets)
    , mBucketFile(bucketFile)
    , mHash(hash)
{
}

VerifyBucketWork::~VerifyBucketWork()
{
}

BasicWork::State
VerifyBucketWork::doWork()
{
    // FIXME (mlo) this a logical problem, as this check shouldn't be here
    // VerifyBucketWork shouldn't be required to know that it depends on
    // GetAndUnzipRemoteFileWork. This will be resolved with batching though,
    // as DownloadBuckets work will take care of when to dispatch download
    // and verify tasks.
    if (!allChildrenSuccessful())
    {
        return BasicWork::WORK_RUNNING;
    }

    if (mDone)
    {
        if (mEc)
        {
            return WORK_FAILURE_RETRY;
        }

        adoptBucket();
        return WORK_SUCCESS;
    }

    spawnVerifier();
    return WORK_WAITING;
}

void
VerifyBucketWork::adoptBucket()
{
    assert(mDone);
    assert(!mEc);

    auto b = mApp.getBucketManager().adoptFileAsBucket(mBucketFile, mHash);
    mBuckets[binToHex(mHash)] = b;
}

void
VerifyBucketWork::spawnVerifier()
{
    std::string filename = mBucketFile;
    uint256 hash = mHash;
    std::weak_ptr<VerifyBucketWork> weak(
        std::static_pointer_cast<VerifyBucketWork>(shared_from_this()));
    auto handler = [weak](asio::error_code const& ec) {
        auto self = weak.lock();
        if (self)
        {
            self->mEc = ec;
            self->mDone = true;
            self->wakeUp();
        }
    };

    mApp.getWorkerIOService().post([filename, handler, hash]() {
        auto hasher = SHA256::create();
        asio::error_code ec;
        char buf[4096];
        {
            // ensure that the stream gets its own scope to avoid race with
            // main thread
            std::ifstream in(filename, std::ifstream::binary);
            while (in)
            {
                in.read(buf, sizeof(buf));
                hasher->add(ByteSlice(buf, in.gcount()));
            }
            uint256 vHash = hasher->finish();
            if (vHash == hash)
            {
                CLOG(DEBUG, "History") << "Verified hash (" << hexAbbrev(hash)
                                       << ") for " << filename;
            }
            else
            {
                CLOG(WARNING, "History")
                    << "FAILED verifying hash for " << filename;
                CLOG(WARNING, "History") << "expected hash: " << binToHex(hash);
                CLOG(WARNING, "History")
                    << "computed hash: " << binToHex(vHash);
                ec = std::make_error_code(std::errc::io_error);
            }
        }
        handler(ec);
    });
}
}
