#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManagerImpl.h"
#include "main/ApplicationImpl.h"

namespace stellar
{

class LedgerManagerEditableVersion : public LedgerManagerImpl
{
  public:
    LedgerManagerEditableVersion(Application& app);

    uint32_t getCurrentLedgerVersion() const override;
    void setCurrentLedgerVersion(uint32_t currentLedgerVersion);

  private:
    uint32_t mCurrentLedgerVersion;
};

class ApplicationEditableVersion : public ApplicationImpl
{
  public:
    ApplicationEditableVersion(VirtualClock& clock, Config const& cfg);

    virtual LedgerManagerEditableVersion& getLedgerManager() override;
};

time_t getTestDate(int day, int month, int year);
std::tm getTestDateTime(int day, int month, int year, int hour, int minute,
                        int second);
}
