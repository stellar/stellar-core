#ifndef __TIMER__
#define __TIMER__

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
