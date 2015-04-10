// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <thread>
#include <cassert>

namespace stellar
{
static std::thread::id mainThread = std::this_thread::get_id();

void assertThreadIsMain()
{
    assert(mainThread == std::this_thread::get_id());
}

}