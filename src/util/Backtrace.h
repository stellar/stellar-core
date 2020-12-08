#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

// Idempotent RAII guard object that sets up and tears down a static
// malloc-buffer used for backtrace-printing. Should establish one of these
// before calling printCurrentBacktrace(), though it will do so itself if you
// forget to. If missing there's a slightly larger chance of backtrace failure
// because of malloc() happening during a backtrace.
class BacktraceManager
{
    bool doFree{false};

  public:
    BacktraceManager();
    ~BacktraceManager();
};

void printCurrentBacktrace();
}
