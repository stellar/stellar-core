#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"

namespace stellar
{

LedgerKey dataKey(AccountID accountID, std::string name);

class DataFrame : public EntryFrame
{
  public:
    explicit DataFrame(AccountID accountID, std::string name, DataValue value);
    explicit DataFrame(LedgerEntry entry);

    DataValue getValue() const;
    void setValue(DataValue value);
};
}
