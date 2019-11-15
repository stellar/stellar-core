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
// and catchup to that instead of destination ledger. This is useful when
// doing offline commandline catchups with stellar-core catchup command.
//
// Catchup can be done in two modes - ONLINE nad OFFLINE. In ONLINE mode node
// is connected to the network. If receives ledgers during catchup and applies
// them after history is applied. Also additional closing ledger is required
// to mark catchup as complete and node as synced. In OFFLINE mode node is not
// connected to network, so new ledgers are not being externalized. Only
// buckets and transactions from history archives are applied.
class CatchupConfiguration
{
  public:
    enum class Mode
    {
        // Do validity checks only on files used for catchup
        OFFLINE_BASIC,
        // Do validity checks on all history archive file types for a given
        // range, regardless of whether files are used or not
        OFFLINE_COMPLETE,
        ONLINE
    };
    static const uint32_t CURRENT = 0;

    CatchupConfiguration(uint32_t toLedger, uint32_t count, Mode mode);
    CatchupConfiguration(LedgerNumHashPair ledgerHashPair, uint32_t count,
                         Mode mode);

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

    Mode
    mode() const
    {
        return mMode;
    }

    bool
    offline() const
    {
        return mMode == Mode::OFFLINE_BASIC || mMode == Mode::OFFLINE_COMPLETE;
    }

    bool
    online() const
    {
        return mMode == Mode::ONLINE;
    }

  private:
    uint32_t mCount;
    LedgerNumHashPair mLedgerHashPair;
    Mode mMode;
};

uint32_t parseLedger(std::string const& str);
uint32_t parseLedgerCount(std::string const& str);
}
