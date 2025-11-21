#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <array>
#include <atomic>
#include <cstdint>

// This is a small utility class used in tests to mark coverage points
// that are otherwise hard to verify via external observation.
//
// It is similar to the mechanism described here:
// https://ferrous-systems.com/blog/coverage-marks/
//
// But it has a few differences:
//
//   1. It is only enabled in test builds
//
//   2. It keeps all atomics in a single global array rather than scattered
//      among a bunch of different variables.
//
//   3. This allows us to do reset all counters to zero at the beginning of
//      each test run, and also do complex checks if we want to, e.g. verify
//      that certain marks were hit in a certain order, or a certain number of
//      times, or different marks sum up to some value, etc.
//
//   4. It also allows us to hash all the marks together to get a trajectory
//      summmary for a run, which we can feed to a fuzzer to explore different
//      code paths.

#ifdef BUILD_TESTS

namespace stellar
{

enum CovMark : std::size_t
{
    EVICTION_TTL_MODIFIED_BETWEEN_DECISION_AND_EVICTION = 0,
    COVMARK_COUNT // This must be the last entry
};

class CovMarks
{
    std::array<std::atomic<std::uint64_t>, CovMark::COVMARK_COUNT> mCovMarks;

  public:
    CovMarks();
    void hit(CovMark mark);
    std::uint64_t get(CovMark mark) const;
    void reset();
    std::uint64_t hash() const;
};

extern CovMarks gCovMarks;

class CovMarkGuard final
{
    CovMark mMark;
    std::uint64_t mValueOnEntry{0};
    char const* mFile;
    int mLine;
    char const* mName;

  public:
    CovMarkGuard(CovMark mark, char const* file, int line, char const* name);
    ~CovMarkGuard() noexcept(false);
};
}

#define COVMARK_HIT(covmark) ::stellar::gCovMarks.hit(CovMark::covmark);
#define COVMARK_CHECK_HIT_IN_CURR_SCOPE(covmark) \
    ::stellar::CovMarkGuard covmark_guard_##covmark##_( \
        CovMark::covmark, __FILE__, __LINE__, #covmark);

#else                          // !BUILD_TESTS
#define COVMARK_HIT(covmark)   // no-op
#define COVMARK_CHECK_HIT_IN_CURR_SCOPE(covmark) // no-op
#endif                         // BUILD_TESTS
