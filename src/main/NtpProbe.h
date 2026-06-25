#pragma once

// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace medida
{
class Counter;
class Meter;
}

namespace stellar
{

class Application;

// Periodically queries an NTP server to estimate the offset between this node's
// wall clock and true time, and warns when the local clock looks significantly
// skewed. The actual SNTP exchange is delegated to a small Rust crate (rsntp)
// via the Rust bridge; the query runs on a background thread (it blocks) and
// only happens about every ten minutes.
class NtpProbe : public std::enable_shared_from_this<NtpProbe>
{
  public:
    static std::shared_ptr<NtpProbe> create(Application& app);

    // Schedule the first probe. Subsequent probes reschedule themselves.
    void start();

    // Stop rescheduling. Any in-flight background query simply discards its
    // result.
    void shutdown();

  private:
    explicit NtpProbe(Application& app);

    void scheduleNext(std::chrono::seconds delay);
    // Kick off a query on a background thread (the SNTP call blocks).
    void beginProbe();
    // Handle a query result back on the main thread.
    void onResult(bool succeeded, int64_t offsetMs);

    Application& mApp;
    std::string const mServer;
    VirtualTimer mProbeTimer;

    // Signed offset to add to the local clock to match true time, in
    // milliseconds (positive => local clock is behind true time).
    medida::Counter& mOffsetMs;
    // Marked whenever a probe fails to produce a usable measurement (DNS
    // failure, timeout, unreachable server).
    medida::Meter& mProbeFailure;

    std::atomic<bool> mShutdown{false};
};
}
