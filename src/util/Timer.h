#ifndef __TIMER__
#define __TIMER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <chrono>
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

namespace stellar
{
    typedef asio::basic_waitable_timer<std::chrono::steady_clock> Timer;
}

#endif
