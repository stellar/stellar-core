#pragma once
// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "rust/RustBridge.h"
#include "util/NonCopyable.h"
#include "xdrpp/types.h"

#include <chrono>
#include <cstdint>
#include <deque>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace stellar
{
class Application;

// This class encapsulates a multithreaded strategy for loading contracts
// out of the database (on one thread) and compiling them (on N-1 others).
class SharedModuleCacheCompiler : NonMovableOrCopyable
{
    ::rust::Box<stellar::rust_bridge::SorobanModuleCache> mModuleCache;
    stellar::SearchableSnapshotConstPtr mSnap;
    std::deque<xdr::xvector<uint8_t>> mWasms;

    size_t const mNumThreads;
    std::vector<std::thread> mThreads;
    // The loading thread will pause and wait for the compiling threads to catch
    // up when it's more than BUFFERED_WASM_CAPACITY bytes ahead of them.
    static size_t const BUFFERED_WASM_CAPACITY;
    bool mLoadedAll{false};
    size_t mBytesLoaded{0};
    size_t mBytesCompiled{0};
    size_t mContractsCompiled{0};
    std::vector<uint32_t> mLedgerVersions;

    std::mutex mMutex;
    std::condition_variable mHaveSpace;
    std::condition_variable mHaveContracts;

    std::chrono::steady_clock::time_point mStarted;
    std::chrono::nanoseconds mTotalCompileTime{0};

    void setFinishedLoading(size_t nContracts);
    bool isFinishedCompiling(std::unique_lock<std::mutex>& lock);
    // This gets called in a loop on the loader/producer thread.
    void pushWasm(xdr::xvector<uint8_t> const& vec);
    // This gets called in a loop on the compiler/consumer threads. It returns
    // true if anything was actually compiled.
    bool popAndCompileWasm(size_t thread, std::unique_lock<std::mutex>& lock);

  public:
    SharedModuleCacheCompiler(SearchableSnapshotConstPtr snap,
                              size_t numThreads,
                              std::vector<uint32_t> const& ledgerVersions);
    ~SharedModuleCacheCompiler();
    void start();
    ::rust::Box<stellar::rust_bridge::SorobanModuleCache> wait();
    size_t getBytesCompiled();
    std::chrono::nanoseconds getCompileTime();
    size_t getContractsCompiled();
};
}