// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef BUILD_TESTS

#include "test/CovMark.h"
#include "crypto/ShortHash.h"

#include <fmt/format.h>
#include <stdexcept>

namespace stellar
{

CovMarks gCovMarks;

CovMarks::CovMarks()
{
    reset();
}

void
CovMarks::hit(CovMark mark)
{
    mCovMarks[static_cast<std::uint64_t>(mark)].fetch_add(
        1, std::memory_order_relaxed);
}

std::uint64_t
CovMarks::get(CovMark mark) const
{
    return mCovMarks[static_cast<std::uint64_t>(mark)].load(
        std::memory_order_relaxed);
}

void
CovMarks::reset()
{
    for (auto& mark : mCovMarks)
    {
        mark.store(0, std::memory_order_relaxed);
    }
}

std::uint64_t
CovMarks::hash() const
{
    std::array<std::uint8_t, sizeof(std::uint64_t) * CovMark::COVMARK_COUNT>
        markBytes;
    for (size_t i = 0; i < CovMark::COVMARK_COUNT; ++i)
    {
        auto mark = mCovMarks[i].load(std::memory_order_relaxed);
        for (size_t j = 0; j < sizeof(std::uint64_t); ++j)
        {
            markBytes[i * sizeof(std::uint64_t) + j] = (mark >> (j * 8)) & 0xFF;
        }
    }
    return shortHash::computeHash(
        ByteSlice(markBytes.data(), markBytes.size()));
}

CovMarkGuard::CovMarkGuard(CovMark mark, char const* file, int line,
                           char const* name)
    : mMark(mark)
    , mValueOnEntry(gCovMarks.get(mark))
    , mFile(file)
    , mLine(line)
    , mName(name)
{
}

CovMarkGuard::~CovMarkGuard() noexcept(false)
{
    auto valueOnExit = gCovMarks.get(mMark);
    // We only throw if we are not already unwinding due to another exception.
    if (!(valueOnExit > mValueOnEntry) && std::uncaught_exceptions() == 0)
    {
        throw std::runtime_error(fmt::format(
            "expected mark '{}' not hit in scope {}:{}", mFile, mLine, mName));
    }
}
}

#endif // BUILD_TESTS