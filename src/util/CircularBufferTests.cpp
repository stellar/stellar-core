// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/CircularBuffer.h"
#include <lib/catch.hpp>

namespace stellar
{

TEST_CASE("initially empty", "[circular_buffer]")
{
    auto buffer = CircularBuffer<int>(1);

    REQUIRE(buffer.empty() == true);
}

TEST_CASE("not empty after push", "[circular_buffer]")
{
    auto buffer = CircularBuffer<int>(1);
    buffer.push(1);

    REQUIRE(buffer.empty() == false);
}

TEST_CASE("pushed element can be popped", "[circular_buffer]")
{
    auto buffer = CircularBuffer<int>(1);
    buffer.push(1);
    int pop = buffer.pop();

    REQUIRE(pop == 1);
}

TEST_CASE("is first-in-first-out", "[circular_buffer]")
{
    auto buffer = CircularBuffer<int>(2);
    buffer.push(1);
    buffer.push(2);

    REQUIRE(buffer.pop() == 1);
    REQUIRE(buffer.pop() == 2);
}

TEST_CASE("is wrapping after filled", "[circular_buffer]")
{
    auto buffer = CircularBuffer<int>(2);
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);

    REQUIRE(buffer.pop() == 2);
    REQUIRE(buffer.pop() == 3);
}

TEST_CASE("empty after all elements popped", "[circular_buffer]")
{
    auto buffer = CircularBuffer<int>(2);
    buffer.push(1);
    buffer.push(2);

    buffer.pop();
    buffer.pop();

    REQUIRE(buffer.empty() == true);
}
}
