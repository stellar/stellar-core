// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/SharedModuleCacheCompiler.h"
#include "bucket/SearchableBucketList.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "rust/RustBridge.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger-entries.h"
#include <chrono>
#include <cstddef>
#include <unordered_set>

namespace stellar
{

size_t const SharedModuleCacheCompiler::BUFFERED_WASM_CAPACITY =
    100 * 1024 * 1024;

SharedModuleCacheCompiler::SharedModuleCacheCompiler(
    SearchableSnapshotConstPtr snap, size_t numThreads,
    std::vector<uint32_t> const& ledgerVersions)
    : mModuleCache(rust_bridge::new_module_cache())
    , mSnap(snap)
    , mNumThreads(numThreads)
    , mLedgerVersions(ledgerVersions)
    , mStarted(std::chrono::steady_clock::now())
{
}

SharedModuleCacheCompiler::~SharedModuleCacheCompiler()
{
    for (auto& t : mThreads)
    {
        t.join();
    }
}

void
SharedModuleCacheCompiler::pushWasm(xdr::xvector<uint8_t> const& vec)
{
    std::unique_lock<std::mutex> lock(mMutex);
    mHaveSpace.wait(lock, [&] {
        return mBytesLoaded - mBytesCompiled < BUFFERED_WASM_CAPACITY;
    });
    xdr::xvector<uint8_t> buf(vec);
    auto size = buf.size();
    mWasms.emplace_back(std::move(buf));
    mBytesLoaded += size;
    lock.unlock();
    mHaveContracts.notify_all();
    LOG_DEBUG(DEFAULT_LOG, "Loaded contract with {} bytes of wasm code", size);
}

bool
SharedModuleCacheCompiler::isFinishedCompiling(
    std::unique_lock<std::mutex>& lock)
{
    releaseAssert(lock.owns_lock());
    return mLoadedAll && mBytesCompiled == mBytesLoaded;
}

void
SharedModuleCacheCompiler::setFinishedLoading(size_t nContracts)
{
    std::unique_lock lock(mMutex);
    mLoadedAll = true;
    mContractsCompiled = nContracts;
    lock.unlock();
    mHaveContracts.notify_all();
}

bool
SharedModuleCacheCompiler::popAndCompileWasm(size_t thread,
                                             std::unique_lock<std::mutex>& lock)
{
    ZoneScoped;

    releaseAssert(lock.owns_lock());

    // Wait for a new contract to compile (or being done).
    mHaveContracts.wait(
        lock, [&] { return !mWasms.empty() || isFinishedCompiling(lock); });

    // Check to see if we were woken up due to end-of-compilation.
    if (isFinishedCompiling(lock))
    {
        return false;
    }

    xdr::xvector<uint8_t> wasm = std::move(mWasms.front());
    mWasms.pop_front();
    lock.unlock();

    auto start = std::chrono::steady_clock::now();
    auto slice = rust::Slice<const uint8_t>(wasm.data(), wasm.size());
    auto hash = binToHex(sha256(wasm));
    for (auto ledgerVersion : mLedgerVersions)
    {
        try
        {
            mModuleCache->compile(ledgerVersion, slice);
        }
        catch (std::exception const& e)
        {
            LOG_FATAL(DEFAULT_LOG,
                      "Thread {} failed to compile {} byte wasm {} for "
                      "protocol {}: {}",
                      thread, wasm.size(), hash, ledgerVersion, e.what());
            throw;
        }
    }
    auto end = std::chrono::steady_clock::now();
    auto dur_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    LOG_DEBUG(DEFAULT_LOG,
              "Thread {} compiled {} byte wasm contract {} in {}us", thread,
              wasm.size(), hash, dur_us.count());
    lock.lock();
    mTotalCompileTime += dur_us;
    mBytesCompiled += wasm.size();
    wasm.clear();
    // Commonly we'd unlock the mutex here before notifying, but we need the
    // `isFinishedCompiling` predicate in the wait calls (and the loop control
    // calling this function) to hold the lock, so to simplify atomicity of the
    // check we both enter _and leave_ this function with the lock held.
    // Possibly there's a better way to organize this but I tried several and
    // kept causing lost wakeups or deadlocks.
    mHaveSpace.notify_all();
    mHaveContracts.notify_all();
    return true;
}

void
SharedModuleCacheCompiler::start()
{
    mStarted = std::chrono::steady_clock::now();

    LOG_INFO(DEFAULT_LOG,
             "Launching 1 loading and {} compiling background threads",
             mNumThreads - 1);

    mThreads.emplace_back(std::thread([this]() {
        ZoneScopedN("load wasm contracts");
        std::unordered_set<Hash> seenContracts;
        this->mSnap->scanForContractCode([&](BucketEntry const& entry) {
            Hash h;
            switch (entry.type())
            {
            case INITENTRY:
            case LIVEENTRY:
                h = entry.liveEntry().data.contractCode().hash;
                if (seenContracts.find(h) == seenContracts.end())
                {
                    this->pushWasm(entry.liveEntry().data.contractCode().code);
                }
                break;
            case DEADENTRY:
                h = entry.deadEntry().contractCode().hash;
                break;
            default:
                break;
            }
            seenContracts.insert(h);
            return Loop::INCOMPLETE;
        });
        this->setFinishedLoading(seenContracts.size());
    }));

    for (auto thread = 1; thread < this->mNumThreads; ++thread)
    {
        mThreads.emplace_back(std::thread([this, thread]() {
            ZoneScopedN("compile wasm contracts");
            size_t nContractsCompiled = 0;
            std::unique_lock<std::mutex> lock(this->mMutex);
            while (!this->isFinishedCompiling(lock))
            {
                if (this->popAndCompileWasm(thread, lock))
                {
                    ++nContractsCompiled;
                }
            }
            LOG_DEBUG(DEFAULT_LOG, "Thread {} compiled {} contracts", thread,
                      nContractsCompiled);
        }));
    }
}

::rust::Box<stellar::rust_bridge::SorobanModuleCache>
SharedModuleCacheCompiler::wait()
{
    std::unique_lock lock(mMutex);
    mHaveContracts.wait(
        lock, [this, &lock] { return this->isFinishedCompiling(lock); });

    auto end = std::chrono::steady_clock::now();
    LOG_INFO(
        DEFAULT_LOG,
        "Compiled {} contracts ({} bytes of Wasm) in {}ms real time, {}ms "
        "CPU time",
        mContractsCompiled, mBytesCompiled,
        std::chrono::duration_cast<std::chrono::milliseconds>(end - mStarted)
            .count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(mTotalCompileTime)
            .count());
    return mModuleCache->shallow_clone();
}

size_t
SharedModuleCacheCompiler::getBytesCompiled()
{
    std::unique_lock lock(mMutex);
    return mBytesCompiled;
}

std::chrono::nanoseconds
SharedModuleCacheCompiler::getCompileTime()
{
    std::unique_lock lock(mMutex);
    return mTotalCompileTime;
}

size_t
SharedModuleCacheCompiler::getContractsCompiled()
{
    std::unique_lock lock(mMutex);
    return mContractsCompiled;
}

}
