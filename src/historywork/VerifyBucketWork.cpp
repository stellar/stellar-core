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

#include <fstream>

namespace stellar
{

VerifyBucketWork::VerifyBucketWork(
    Application& app, WorkParent& parent,
    std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::string const& bucketFile, uint256 const& hash)
    : Work(app, parent, std::string("verify-bucket-hash ") + bucketFile,
           RETRY_NEVER)
    , mBuckets(buckets)
    , mBucketFile(bucketFile)
    , mHash(hash)
{
    fs::checkNoGzipSuffix(mBucketFile);
}

void
VerifyBucketWork::onStart()
{
    std::string filename = mBucketFile;
    uint256 hash = mHash;
    Application& app = this->mApp;
    auto handler = callComplete();
    app.getWorkerIOService().post([&app, filename, handler, hash]() {
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
                hasher->add(ByteSlice(buf, static_cast<uint64_t>(in.gcount())));
            }
            uint256 vHash = hasher->finish();
            if (vHash == hash)
            {
                CLOG(DEBUG, "History") << "Verified hash (" << hexAbbrev(hash)
                                       << ") for " << filename;
            }
            else
            {
                CLOG(WARNING, "History") << "FAILED verifying hash for "
                                         << filename;
                CLOG(WARNING, "History") << "expected hash: " << binToHex(hash);
                CLOG(WARNING, "History") << "computed hash: "
                                         << binToHex(vHash);
                ec = std::make_error_code(std::errc::io_error);
            }
        }
        app.getClock().getIOService().post([ec, handler]() { handler(ec); });
    });
}

void
VerifyBucketWork::onRun()
{
    // Do nothing: we spawned the verifier in onStart().
}

Work::State
VerifyBucketWork::onSuccess()
{
    auto b = mApp.getBucketManager().adoptFileAsBucket(mBucketFile, mHash);
    mBuckets[binToHex(mHash)] = b;
    return WORK_SUCCESS;
}
}
