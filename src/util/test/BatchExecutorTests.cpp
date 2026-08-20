// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/Catch2.h"
#include "util/BatchExecutor.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

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
