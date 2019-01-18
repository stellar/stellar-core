#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerRange.h"
#include <cstdint>
#include <string>

namespace stellar
{

// Each catchup can be configured by two parameters destination ledger
// (and its hash, if known) and count of ledgers to apply.
// Value of count can be adjusted in different ways during catchup. If applying
// count ledgers would mean going before the last closed ledger - it is
// reduced. Otherwise it can be slightly enlarged (by at most checkpoint
// frequency value) to ensure, that at least count ledgers are applied.
//
// Different values of CatchupConfiguration give the same results as old
// catchup modes:
// - CATCHUP_MINIMAL - count set to 0
// - CATCHUP_COMPLETE - count set to UINT32_MAX
// - CATCHUP_RECENT - count set to recent value
//
// Value of destination ledger can be also set to CatchupConfiguration::CURRENT
// which means that CatchupWork will get latest checkpoint from history archive
// and catchup to that instead of destination ledger. This is usefull when
// doing offline commandline catchups with stellar-core catchup command.
class CatchupConfiguration
{
  public:
    static const uint32_t CURRENT = 0;

    CatchupConfiguration(uint32_t toLedger, uint32_t count);
    CatchupConfiguration(LedgerNumHashPair ledgerHashPair, uint32_t count);

    /**
     * If toLedger() == CatchupConfiguration::CURRENT it replaces it with
     * remoteCheckpoint in returned value, if not - returns copy of self.
     */
    CatchupConfiguration resolve(uint32_t remoteCheckpoint) const;

    uint32_t
    toLedger() const
    {
        return mLedgerHashPair.first;
    }

    uint32_t
    count() const
    {
        return mCount;
    }

    optional<Hash>
    hash() const
    {
        return mLedgerHashPair.second;
    }

  private:
    uint32_t mCount;
    LedgerNumHashPair mLedgerHashPair;
};

uint32_t parseLedger(std::string const& str);
uint32_t parseLedgerCount(std::string const& str);
}
