// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/NtpProbe.h"

#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "rust/RustBridge.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"

#include "medida/counter.h"
#include "medida/meter.h"

#include <exception>

namespace stellar
{

namespace
{
constexpr auto NTP_PROBE_INTERVAL = std::chrono::minutes(10);
// First probe fires a little after startup rather than immediately, to avoid
// racing the rest of node bring-up.
constexpr auto NTP_PROBE_INITIAL_DELAY = std::chrono::seconds(30);
constexpr uint64_t NTP_PROBE_TIMEOUT_SECONDS = 5;
constexpr auto NTP_OFFSET_WARN_THRESHOLD = std::chrono::milliseconds(500);

// Make sure we don't crash if our probe fails, since it's just a warning
// diagnostic. Any caught exception is counted as a probe failure and logged.
template <typename F>
void
guardProbeStep(char const* step, medida::Meter& failureMeter, F&& f)
{
    try
    {
        f();
    }
    catch (std::exception const& e)
    {
        failureMeter.Mark();
        CLOG_WARNING(Herder, "NTP probe step '{}' failed: {}", step, e.what());
    }
    catch (...)
    {
        failureMeter.Mark();
        CLOG_WARNING(Herder,
                     "NTP probe step '{}' failed with unknown exception", step);
    }
}
}

std::shared_ptr<NtpProbe>
NtpProbe::create(Application& app)
{
    return std::shared_ptr<NtpProbe>(new NtpProbe(app));
}

NtpProbe::NtpProbe(Application& app)
    : mApp(app)
    , mServer(app.getConfig().NTP_DRIFT_CHECK_SERVER)
    , mProbeTimer(app)
    , mOffsetMs(app.getMetrics().NewCounter({"clock", "ntp", "offset-ms"}))
    , mProbeFailure(
          app.getMetrics().NewMeter({"clock", "ntp", "probe-failure"}, "probe"))
{
}

void
NtpProbe::start()
{
    if (mServer.empty())
    {
        return;
    }
    CLOG_INFO(
        Herder,
        "Clock-drift check enabled, querying NTP server '{}' every {} "
        "minutes (detection only; does not adjust the clock)",
        mServer,
        std::chrono::duration_cast<std::chrono::minutes>(NTP_PROBE_INTERVAL)
            .count());
    scheduleNext(NTP_PROBE_INITIAL_DELAY);
}

void
NtpProbe::shutdown()
{
    mShutdown = true;
    mProbeTimer.cancel();
}

void
NtpProbe::scheduleNext(std::chrono::seconds delay)
{
    if (mShutdown)
    {
        return;
    }
    mProbeTimer.expires_from_now(delay);
    auto self = shared_from_this();
    mProbeTimer.async_wait([self](asio::error_code const& ec) {
        if (!ec && !self->mShutdown)
        {
            guardProbeStep("schedule", self->mProbeFailure,
                           [&]() { self->beginProbe(); });
        }
    });
}

void
NtpProbe::beginProbe()
{
    if (mShutdown)
    {
        return;
    }

    // The SNTP query blocks (network round-trip + timeout), so run it on a
    // background thread and hand the result back to the main thread.
    auto self = shared_from_this();
    std::string const server = mServer;
    mApp.postOnBackgroundThread(
        [self, server]() {
            guardProbeStep("query", self->mProbeFailure, [&]() {
                auto const result = rust_bridge::query_ntp_offset(
                    server, NTP_PROBE_TIMEOUT_SECONDS);
                self->mApp.postOnMainThread(
                    [self, succeeded = result.succeeded,
                     offsetMs = result.offset_millis]() {
                        guardProbeStep("result", self->mProbeFailure, [&]() {
                            self->onResult(succeeded, offsetMs);
                        });
                    },
                    "NtpProbe::onResult");
            });
        },
        "NtpProbe::query");
}

void
NtpProbe::onResult(bool succeeded, int64_t offsetMs)
{
    if (mShutdown)
    {
        return;
    }

    if (!succeeded)
    {
        mProbeFailure.Mark();
        CLOG_INFO(Herder,
                  "NTP probe to '{}' produced no usable measurement (server "
                  "unreachable, timed out, or egress blocked)",
                  mServer);
    }
    else
    {
        mOffsetMs.set_count(offsetMs);
        if (std::chrono::abs(std::chrono::milliseconds(offsetMs)) >
            NTP_OFFSET_WARN_THRESHOLD)
        {
            CLOG_WARNING(Herder, POSSIBLY_BAD_LOCAL_CLOCK);
            CLOG_WARNING(Herder,
                         "Local clock is off by {} ms versus NTP server '{}' "
                         "(positive means the local clock is behind). This can "
                         "degrade ledger close performance; ensure NTP is "
                         "running and healthy.",
                         offsetMs, mServer);
        }
        else
        {
            CLOG_DEBUG(Herder, "NTP offset versus '{}' is {} ms", mServer,
                       offsetMs);
        }
    }

    scheduleNext(NTP_PROBE_INTERVAL);
}
}
