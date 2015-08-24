#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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

#ifndef ASIO_HAS_STD_ARRAY
#define ASIO_HAS_STD_ARRAY
#endif

#ifndef ASIO_HAS_STD_CHRONO
#define ASIO_HAS_STD_CHRONO
#endif

#ifndef ASIO_HAS_STD_ADDRESSOF
#define ASIO_HAS_STD_ADDRESSOF
#endif

#ifndef ASIO_HAS_STD_FUNCTION
#define ASIO_HAS_STD_FUNCTION
#endif

#ifndef ASIO_HAS_STD_SHARED_PTR
#define ASIO_HAS_STD_SHARED_PTR
#endif

#ifndef ASIO_HAS_STD_MUTEX_AND_CONDVAR
#define ASIO_HAS_STD_MUTEX_AND_CONDVAR
#endif

#ifndef ASIO_HAS_STD_ATOMIC
#define ASIO_HAS_STD_ATOMIC
#endif

#ifndef ASIO_HAS_STD_TYPE_TRAITS
#define ASIO_HAS_STD_TYPE_TRAITS
#endif

#ifndef ASIO_HAS_CSTDINT
#define ASIO_HAS_CSTDINT
#endif

#ifndef ASIO_HAS_STD_THREAD
#define ASIO_HAS_STD_THREAD
#endif

#ifndef ASIO_HAS_STD_SYSTEM_ERROR
#define ASIO_HAS_STD_SYSTEM_ERROR
#endif

#include <asio.hpp>
