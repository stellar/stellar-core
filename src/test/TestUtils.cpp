// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestUtils.h"

#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "util/make_unique.h"

namespace stellar
{

LedgerManagerEditableVersion::LedgerManagerEditableVersion(Application& app)
    : LedgerManagerImpl(app)
{
}

uint32_t
LedgerManagerEditableVersion::getCurrentLedgerVersion() const
{
    return mCurrentLedgerVersion;
}

void
LedgerManagerEditableVersion::setCurrentLedgerVersion(
    uint32_t currentLedgerVersion)
{
    mCurrentLedgerVersion = currentLedgerVersion;
}

ApplicationEditableVersion::ApplicationEditableVersion(VirtualClock& clock,
                                                       Config const& cfg)
    : ApplicationImpl(clock, cfg)
{
    mLedgerManager = make_unique<LedgerManagerEditableVersion>(*this);
    mHerder = Herder::create(*this); // need to recreate
    newDB();
}

LedgerManagerEditableVersion&
ApplicationEditableVersion::getLedgerManager()
{
    return static_cast<LedgerManagerEditableVersion&>(*mLedgerManager);
}

time_t
getTestDate(int day, int month, int year)
{
    auto tm = getTestDateTime(day, month, year, 0, 0, 0);

    VirtualClock::time_point tp = VirtualClock::tmToPoint(tm);
    time_t t = VirtualClock::to_time_t(tp);

    return t;
}

std::tm
getTestDateTime(int day, int month, int year, int hour, int minute, int second)
{
    std::tm tm = {0};
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_mday = day;
    tm.tm_mon = month - 1; // 0 based
    tm.tm_year = year - 1900;
    return tm;
}
}
