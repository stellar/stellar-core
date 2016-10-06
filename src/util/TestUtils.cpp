// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestUtils.h"

#include "herder/LedgerCloseData.h"
#include "herder/HerderImpl.h"
#include "util/make_unique.h"

namespace stellar
{

LedgerManagerEditableVersion::LedgerManagerEditableVersion(Application& app) : LedgerManagerImpl(app)
{
}

uint32_t
LedgerManagerEditableVersion::getCurrentLedgerVersion() const
{
    return mCurrentLedgerVersion;
}

void
LedgerManagerEditableVersion::setCurrentLedgerVersion(uint32_t currentLedgerVersion)
{
    mCurrentLedgerVersion = currentLedgerVersion;
}

ApplicationEditableVersion::ApplicationEditableVersion(VirtualClock& clock, Config const& cfg)
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

}
