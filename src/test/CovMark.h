// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Coverage marks: a lightweight mechanism for tests to assert that specific
// internal code paths were actually executed. Inspired by the "coverage marks"
// pattern from Ferrous Systems.
//
// Usage in production code:
//     COVMARK_HIT(SOME_RARE_PATH);
//
// Usage in test code:
//     COVMARK_CHECK_HIT_IN_CURR_SCOPE(SOME_RARE_PATH);
//     // ... code that should trigger SOME_RARE_PATH ...
//
// The check-hit guard records the counter on construction and verifies it
// increased by scope exit. If the marked path was not hit, the test fails.

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <stdexcept>

#ifdef BUILD_TESTS
#include <fmt/format.h>
#endif

namespace stellar
{

// Each coverage mark is an enum value. Add new marks before COVMARK_COUNT.
enum CovMark : std::size_t
{
    BINARY_FUSE_POPULATE_ERROR_RETRY = 0,
    BINARY_FUSE_POPULATE_PEELING_FAILURE_RETRY,
    BINARY_FUSE_DUPLICATE_REMOVAL,
    COVMARK_COUNT // must be last
};

#ifdef BUILD_TESTS

class CovMarks
{
    std::array<std::atomic<std::uint64_t>, CovMark::COVMARK_COUNT> mCounters{};

  public:
    void
    hit(CovMark mark)
    {
        mCounters[mark].fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t
    get(CovMark mark) const
    {
        return mCounters[mark].load(std::memory_order_relaxed);
    }

    void
    reset()
    {
        for (auto& c : mCounters)
        {
            c.store(0, std::memory_order_relaxed);
        }
    }
};

extern CovMarks gCovMarks;

class CovMarkGuard
{
    CovMark mMark;
    std::uint64_t mValueOnEntry;
    char const* mFile;
    int mLine;
    char const* mName;

  public:
    CovMarkGuard(CovMark mark, char const* file, int line, char const* name)
        : mMark(mark)
        , mValueOnEntry(gCovMarks.get(mark))
        , mFile(file)
        , mLine(line)
        , mName(name)
    {
    }

    ~CovMarkGuard() noexcept(false)
    {
        if (std::uncaught_exceptions() == 0)
        {
            auto valueOnExit = gCovMarks.get(mMark);
            if (valueOnExit <= mValueOnEntry)
            {
                throw std::runtime_error(
                    fmt::format("{}:{}: coverage mark '{}' was not hit during "
                                "this scope",
                                mFile, mLine, mName));
            }
        }
    }
};

#define COVMARK_HIT(covmark) \
    ::stellar::gCovMarks.hit(::stellar::CovMark::covmark)

#define COVMARK_CHECK_HIT_IN_CURR_SCOPE(covmark) \
    ::stellar::CovMarkGuard _covMarkGuard_##covmark( \
        ::stellar::CovMark::covmark, __FILE__, __LINE__, #covmark)

#else // !BUILD_TESTS

#define COVMARK_HIT(covmark) ((void)0)
#define COVMARK_CHECK_HIT_IN_CURR_SCOPE(covmark) ((void)0)

#endif // BUILD_TESTS
}
