#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#if defined(_MSC_VER)
#include <soci.h>
#else
#pragma GCC diagnostic push
// soci uses std::auto_ptr internally
// these warnings are useless to us and only clutters the output
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <soci.h>
#pragma GCC diagnostic pop
#endif // !
