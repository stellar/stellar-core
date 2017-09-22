#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class Database;

struct InflationWinner
{
    int64 mVotes;
    AccountID mInflationDest;
};

std::vector<InflationWinner> inflationWinners(int64_t minVotes, int maxWinners,
                                              Database& db);
}
