// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/Catch2.h"
#include "util/BatchExecutor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

using namespace stellar;

namespace
{
template <typename T>
std::vector<std::function<T()>>
makeTasks(int n, std::function<T(int)> const& body)
{
    std::vector<std::function<T()>> tasks;
    tasks.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        tasks.emplace_back([body, i]() { return body(i); });
    }
    return tasks;
}

void
requireValidRangeCoverage(BatchExecutor& exec, size_t count, size_t numTasks)
{
    std::vector<std::atomic<int>> visits(count);
    for (auto& v : visits)
    {
        v.store(0);
    }
    std::mutex rangesMutex;
    // (rangeIndex, begin, end) of every executed range.
    std::vector<std::tuple<size_t, size_t, size_t>> ranges;
    exec.executeBatchOverRanges(
        count, numTasks, [&](size_t begin, size_t end, size_t rangeIndex) {
            for (size_t i = begin; i < end; ++i)
            {
                visits[i].fetch_add(1);
            }
            std::lock_guard<std::mutex> guard(rangesMutex);
            ranges.emplace_back(rangeIndex, begin, end);
        });

    std::sort(ranges.begin(), ranges.end());
    REQUIRE(!ranges.empty());
    REQUIRE(ranges.size() <= std::max<size_t>(numTasks, 1));
    size_t expectedBegin = 0;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        auto const& [rangeIndex, begin, end] = ranges[i];
        REQUIRE(rangeIndex == i);
        REQUIRE(begin == expectedBegin);
        REQUIRE(end >= begin);
        expectedBegin = end;
    }
    REQUIRE(expectedBegin == count);
    for (auto const& v : visits)
    {
        REQUIRE(v.load() == 1);
    }
}
}

TEST_CASE("BatchExecutor basic tests", "[batchexecutor]")
{
    BatchExecutor exec;
    int const batchSize = 7;
    SECTION("empty batch is a no-op")
    {
        auto results = exec.executeBatch<int>({});
        REQUIRE(results.empty());
    }

    SECTION("a single task runs on the calling thread")
    {
        auto callingThread = std::this_thread::get_id();
        auto results = exec.executeBatch(makeTasks<std::thread::id>(
            1, [](int) { return std::this_thread::get_id(); }));
        REQUIRE(results.size() == 1);
        REQUIRE(results[0] == callingThread);

        // Larger batches are still dispatched to the worker threads.
        results = exec.executeBatch(makeTasks<std::thread::id>(
            batchSize, [](int) { return std::this_thread::get_id(); }));
        REQUIRE(results.size() == batchSize);
        for (auto const& id : results)
        {
            REQUIRE(id != callingThread);
        }
    }

    SECTION("results are in task order")
    {
        auto results = exec.executeBatch(
            makeTasks<int>(batchSize, [](int i) { return i * i; }));
        REQUIRE(results.size() == batchSize);
        for (int i = 0; i < batchSize; ++i)
        {
            REQUIRE(results[i] == i * i);
        }
    }

    SECTION("tasks run exactly once")
    {
        std::vector<std::atomic<int>> counts(batchSize);
        for (auto& c : counts)
        {
            c.store(0);
        }
        auto tasks = makeTasks<int>(
            batchSize, [&counts](int i) { return counts[i].fetch_add(1); });
        auto results = exec.executeBatch(std::move(tasks));
        for (int i = 0; i < batchSize; ++i)
        {
            REQUIRE(counts[i].load() == 1);
        }
    }

    SECTION("tasks run concurrently")
    {
        std::atomic<int> arrived{0};
        std::atomic<bool> anyTimedOut{false};
        auto tasks = makeTasks<int>(batchSize, [&](int i) -> int {
            arrived.fetch_add(1);
            auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (arrived.load() < batchSize)
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    anyTimedOut.store(true);
                    break;
                }
            }
            return i;
        });
        auto results = exec.executeBatch(std::move(tasks));
        REQUIRE_FALSE(anyTimedOut.load());
        REQUIRE(arrived.load() == batchSize);
    }

    SECTION("task exceptions are rethrown after all finish")
    {
        std::atomic<int> ran{0};
        auto tasks = makeTasks<int>(batchSize, [&](int i) -> int {
            ran.fetch_add(1);
            if (i == 2)
            {
                throw std::logic_error("error");
            }
            return i;
        });
        REQUIRE_THROWS_AS(exec.executeBatch(std::move(tasks)),
                          std::logic_error);
        REQUIRE(ran.load() == batchSize);

        // Executor is still usable after a batch throws.
        auto results = exec.executeBatch(
            makeTasks<int>(batchSize, [](int i) { return i * i; }));
        REQUIRE(results.size() == batchSize);
        for (int i = 0; i < batchSize; ++i)
        {
            REQUIRE(results[i] == i * i);
        }
    }

    SECTION("ranges tile the whole element range")
    {
        requireValidRangeCoverage(exec, 1, batchSize);
        requireValidRangeCoverage(exec, batchSize, batchSize);
        requireValidRangeCoverage(exec, batchSize + 1, batchSize);
        requireValidRangeCoverage(exec, batchSize * 3, batchSize);
        requireValidRangeCoverage(exec, batchSize * 3 + 1, batchSize);
    }

    SECTION("a single range runs on the calling thread")
    {
        auto callingThread = std::this_thread::get_id();
        auto requireRunsInline = [&](size_t count, size_t numTasks) {
            size_t invocations = 0;
            exec.executeBatchOverRanges(
                count, numTasks,
                [&](size_t begin, size_t end, size_t rangeIndex) {
                    ++invocations;
                    REQUIRE(begin == 0);
                    REQUIRE(end == count);
                    REQUIRE(rangeIndex == 0);
                    REQUIRE(std::this_thread::get_id() == callingThread);
                });
            REQUIRE(invocations == 1);
        };
        requireRunsInline(batchSize, 1);
        requireRunsInline(batchSize - 1, batchSize);
        requireRunsInline(0, batchSize);
    }

    SECTION("ranges run concurrently")
    {
        std::atomic<int> arrived{0};
        std::atomic<bool> anyTimedOut{false};
        exec.executeBatchOverRanges(
            batchSize, batchSize, [&](size_t, size_t, size_t) {
                arrived.fetch_add(1);
                auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (arrived.load() < batchSize)
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        anyTimedOut.store(true);
                        break;
                    }
                }
            });
        REQUIRE_FALSE(anyTimedOut.load());
        REQUIRE(arrived.load() == batchSize);
    }

    SECTION("range exceptions are rethrown after all finish")
    {
        std::atomic<int> ran{0};
        REQUIRE_THROWS_AS(
            exec.executeBatchOverRanges(batchSize, batchSize,
                                        [&](size_t begin, size_t, size_t) {
                                            ran.fetch_add(1);
                                            if (begin == 2)
                                            {
                                                throw std::logic_error("error");
                                            }
                                        }),
            std::logic_error);
        REQUIRE(ran.load() == batchSize);

        // Executor is still usable after a range batch throws.
        requireValidRangeCoverage(exec, batchSize, batchSize);
    }
}

TEST_CASE("BatchExecutor runs many successive batches", "[batchexecutor]")
{
    BatchExecutor exec;
    uniform_int_distribution<> batchDistr(1, 25);
    for (int round = 0; round < 100; ++round)
    {
        int const batchSize = batchDistr(Catch::rng());
        auto results = exec.executeBatch(makeTasks<int>(
            batchSize, [round](int i) { return round * 1000 + i; }));
        for (int i = 0; i < batchSize; ++i)
        {
            REQUIRE(results[i] == round * 1000 + i);
        }
    }
}

TEST_CASE("BatchExecutor runs many successive range batches", "[batchexecutor]")
{
    BatchExecutor exec;
    uniform_int_distribution<> countDistr(0, 40);
    uniform_int_distribution<> taskDistr(1, 12);
    for (int round = 0; round < 100; ++round)
    {
        requireValidRangeCoverage(exec, countDistr(Catch::rng()),
                                  taskDistr(Catch::rng()));
    }
}
