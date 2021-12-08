// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/FlushAndRotateMetaDebugWork.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include <filesystem>
#include <memory>
#include <regex>
#include <stdexcept>
#include <system_error>

namespace
{
const std::string META_DEBUG_DIRNAME{"meta-debug"};
const std::string META_DEBUG_FILE_FMT_STR{"meta-debug-{:08x}-{}.xdr"};
const std::regex META_DEBUG_FILE_REGEX{
    "meta-debug-[[:xdigit:]]+-[[:xdigit:]]+\\.xdr(\\.gz)?"};

// This number can be changed in the future without any coordination,
// it just controls the granularity of new meta-debug XDR segments.
//
// 256 ledgers == ~21 minutes. At time of writing, ~5mb meta / minute
// gives ~105mb meta / segment, which should compress to ~20mb.
const uint32_t META_DEBUG_LEDGER_SEGMENT_SIZE = 256;
}

namespace stellar
{

FlushAndRotateMetaDebugWork::FlushAndRotateMetaDebugWork(
    Application& app, std::filesystem::path const& metaDebugPath,
    std::unique_ptr<XDROutputFileStream> metaDebugFile, uint32_t ledgersToKeep)
    : Work(app, "flush and rotate meta-debug", BasicWork::RETRY_NEVER)
    , mMetaDebugPath(metaDebugPath)
    , mMetaDebugFile(std::move(metaDebugFile))
    , mLedgersToKeep(ledgersToKeep)
{
}

std::filesystem::path
FlushAndRotateMetaDebugWork::getMetaDebugDirPath(
    std::filesystem::path const& bucketDir)
{
    return bucketDir / META_DEBUG_DIRNAME;
}

std::filesystem::path
FlushAndRotateMetaDebugWork::getMetaDebugFilePath(
    std::filesystem::path const& bucketDir, uint32_t seqNum)
{
    auto file =
        fmt::format(META_DEBUG_FILE_FMT_STR, seqNum, binToHex(randomBytes(8)));
    return getMetaDebugDirPath(bucketDir) / file;
}

std::vector<std::filesystem::path>
FlushAndRotateMetaDebugWork::listMetaDebugFiles(
    std::filesystem::path const& bucketDir)
{
    auto dir = getMetaDebugDirPath(bucketDir);
    auto files = fs::findfiles(dir.string(), [](std::string const& file) {
        return std::regex_match(file, META_DEBUG_FILE_REGEX);
    });
    std::sort(files.begin(), files.end());
    return std::vector<std::filesystem::path>(files.begin(), files.end());
}

bool
FlushAndRotateMetaDebugWork::isDebugSegmentBoundary(uint32_t ledgerSeq)
{
    if (META_DEBUG_LEDGER_SEGMENT_SIZE == 1)
    {
        return true;
    }
    return ledgerSeq % (META_DEBUG_LEDGER_SEGMENT_SIZE - 1) == 0;
}

size_t
FlushAndRotateMetaDebugWork::getNumberOfDebugFilesToKeep(uint32_t numLedgers)
{
    size_t segLen = META_DEBUG_LEDGER_SEGMENT_SIZE;
    return (numLedgers + segLen - 1) / segLen;
}

BasicWork::State
FlushAndRotateMetaDebugWork::doWork()
{
    // Step 1: transfer ownership of mMetaDebugFile to background thread
    // and flush/fsync it. When that completes, it will post an action
    // back to the main thread that unblocks this work to continue with
    // gzip'ing and rotating.
    if (mMetaDebugFile)
    {
        std::weak_ptr<FlushAndRotateMetaDebugWork> weak =
            std::static_pointer_cast<FlushAndRotateMetaDebugWork>(
                shared_from_this());

        // This is a little silly, but we have ownership of a non-copyable
        // unique_ptr here, and we want to transfer that ownership into a
        // closure held by a std::function -- which unfortunately requires
        // its body to be copy-constructable. So we double-indirect through
        // something copy-constructble: a shared_ptr<unique_ptr<...>>.
        using OwnedStream = std::unique_ptr<XDROutputFileStream>;
        auto file = std::make_shared<OwnedStream>(std::move(mMetaDebugFile));
        mApp.postOnBackgroundThread(
            [weak, file]() {
                auto self = weak.lock();
                if (!self || self->isAborting())
                {
                    return;
                }

                // First close() here, which will call fsync(), which blocks.
                CLOG_DEBUG(Ledger, "closing meta-debug file {}",
                           self->mMetaDebugPath.string());
                try
                {
                    (*file)->close();
                }
                catch (std::runtime_error& e)
                {
                    // If we fail to close here, we're going to just eat the
                    // error and carry on to trying to gzip. Hopefully this
                    // doesn't cause a filehandle leak or something similarly
                    // horrible.
                    CLOG_WARNING(Ledger,
                                 "Failed to close debug metadata stream: {}",
                                 e.what());
                }

                // Then post back to main thread.
                self->mApp.postOnMainThread(
                    [weak]() {
                        auto self = weak.lock();
                        if (self)
                        {
                            self->wakeUp();
                        }
                    },
                    "wake up gzip and rotate meta-debug");
            },
            "close and fsync meta-debug");
        return BasicWork::State::WORK_WAITING;
    }

    // Step 2: wait for the creation and completion of mGzipFileWork.
    if (!mGzipFileWork)
    {
        CLOG_DEBUG(Ledger, "compressing meta-debug file {}",
                   mMetaDebugPath.string());
        mGzipFileWork = addWork<GzipFileWork>(mMetaDebugPath.string());
        return BasicWork::State::WORK_RUNNING;
    }
    if (!mGzipFileWork->isDone())
    {
        return mGzipFileWork->getState();
    }
    if (mGzipFileWork->getState() == State::WORK_SUCCESS)
    {
        CLOG_DEBUG(Ledger, "compressed meta-debug file {}",
                   mMetaDebugPath.string());
    }
    else
    {
        CLOG_ERROR(Ledger, "failed to compress meta-debug file {}",
                   mMetaDebugPath.string());
        return State::WORK_FAILURE;
    }

    // Step 3: synchronously rotate the meta files in the directory, whether
    // gzipped or not -- non-gzipped ones are left behind when users kill or
    // restart core processes.
    auto bucketDir = mApp.getBucketManager().getBucketDir();
    auto dir = getMetaDebugDirPath(bucketDir);
    auto files = listMetaDebugFiles(bucketDir);
    auto keep = getNumberOfDebugFilesToKeep(mLedgersToKeep);
    if (files.size() > keep)
    {
        // Forget about the most-recent (highest-sorting) keep files -- they get
        // to survive.
        files.resize(files.size() - keep);

        // Delete all (size()-keep) younger (lower-sorting) files not forgotten
        // above.
        for (auto const& file : files)
        {
            releaseAssert(
                std::regex_match(file.string(), META_DEBUG_FILE_REGEX));
            auto f = dir / file;
            CLOG_DEBUG(Ledger, "trimming old meta-debug file {}", f.string());
            std::error_code ec;
            std::filesystem::remove(f, ec);
            // Ignore errors: there's nothing we can do to "try harder" and
            // failing the work is not helpful. We'll just try again.
        }
    }
    return State::WORK_SUCCESS;
}

}
