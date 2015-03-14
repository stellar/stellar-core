#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(warn_unused_result)
#define MUST_USE __attribute__((warn_unused_result))
#else
#define MUST_USE
#endif
