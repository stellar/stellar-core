#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{
class Database;
struct LedgerKey;

std::string checkAgainstDatabase(LedgerEntry const& entry, Database& db);
LedgerKey entryKey(LedgerEntry const& e);

class EntryFrame
{
  public:
    EntryFrame() = default;
    explicit EntryFrame(LedgerEntry entry);
    virtual ~EntryFrame() = default;

    LedgerKey getKey() const;

    LedgerEntry const&
    getEntry() const
    {
        return mEntry;
    }

    LedgerEntry&
    getEntry()
    {
        return mEntry;
    }

  protected:
    LedgerEntry mEntry;
};
}
