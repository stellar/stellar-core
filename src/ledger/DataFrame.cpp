// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/DataFrame.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"

namespace stellar
{

LedgerKey dataKey(AccountID accountID, std::string name)
{
    auto k = LedgerKey{};
    k.type(DATA);
    k.data().accountID = std::move(accountID);
    k.data().dataName = std::move(name);
    return k;
}

DataFrame::DataFrame(AccountID accountID, std::string name,
                     stellar::DataValue value)
{
    mEntry.data.type(DATA);
    mEntry.data.data().accountID = std::move(accountID);
    mEntry.data.data().dataName = std::move(name);
    mEntry.data.data().dataValue = std::move(value);
}

DataFrame::DataFrame(LedgerEntry entry) : EntryFrame{std::move(entry)}
{
    assert(mEntry.data.type() == DATA);
}

DataValue
DataFrame::getValue() const
{
    return mEntry.data.data().dataValue;
}

void
DataFrame::setValue(stellar::DataValue value)
{
    mEntry.data.data().dataValue = std::move(value);
}
}
