#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(warn_unused_result)
#define MUST_USE __attribute__((warn_unused_result))
#else
#define MUST_USE
#endif
