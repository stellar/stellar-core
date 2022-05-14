#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO does platform-support feature autoconfig and if it's building on
// libstdc++ this will only work if _some_ standard library component is
// included before its autoconfig machinery, so that <bits/c++config.h> gets
// pulled in and defines a bunch of _GLIBCXX_HAVE_FOO macros. We include
// system_error here as the most trivial such component
#include <system_error>

#if defined(ASIO_WINDOWS_RUNTIME)
// Empty.
#elif defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#error `asio.h` is somewhat particular about when it gets included -- it wants to be the first to include <windows.h>. Include it before everything else.
#endif

#ifndef ASIO_SEPARATE_COMPILATION
#define ASIO_SEPARATE_COMPILATION
#endif

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <asio.hpp>
