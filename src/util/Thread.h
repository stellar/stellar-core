#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <chrono>
#include <future>
#include <thread>

namespace stellar
{

void runCurrentThreadWithLowPriority();

template <typename T>
bool
futureIsReady(std::future<T> const& fut)
{
    if (!fut.valid())
    {
        return false;
    }
    auto status = fut.wait_for(std::chrono::nanoseconds(1));
    return status == std::future_status::ready;
}

template <typename T>
bool
futureIsReady(std::shared_future<T> const& fut)
{
    if (!fut.valid())
    {
        return false;
    }
    auto status = fut.wait_for(std::chrono::nanoseconds(1));
    return status == std::future_status::ready;
}
}
