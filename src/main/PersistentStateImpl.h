#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/PersistentState.h"

namespace stellar
{

class PersistentStateImpl : public PersistentState
{
  public:
    PersistentStateImpl(Application& app);

    std::string getState(Entry stateName) override;
    void setState(Entry stateName, std::string const& value) override;
    std::vector<std::string> getSCPStateAllSlots() override;
    void setSCPStateForSlot(uint64 slot, std::string const& value) override;

  private:
    Application& mApp;

    std::string getStoreStateName(Entry n, uint32 subscript = 0);
    void updateDb(std::string const& entry, std::string const& value);
    std::string getFromDb(std::string const& entry);
};
}
