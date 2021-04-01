#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSetFrame.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "util/optional.h"
#include <string>

namespace stellar
{

/**
 * Helper class that describes a single ledger-to-close -- a set of transactions
 * and auxiliary values -- as decided by the Herder (and ultimately: SCP). This
 * does not include the effects of _performing_ any transactions, merely the
 * values that the network has agreed _to apply_ to the current ledger,
 * atomically, in order to produce the next ledger.
 */
class LedgerCloseData
{
  public:
    LedgerCloseData(uint32_t ledgerSeq,
                    std::shared_ptr<AbstractTxSetFrameForApply> txSet,
                    StellarValue const& v,
                    optional<Hash> const& expectedLedgerHash);

    LedgerCloseData(uint32_t ledgerSeq,
                    std::shared_ptr<AbstractTxSetFrameForApply> txSet,
                    StellarValue const& v);

    uint32_t
    getLedgerSeq() const
    {
        return mLedgerSeq;
    }
    std::shared_ptr<AbstractTxSetFrameForApply>
    getTxSet() const
    {
        return mTxSet;
    }
    StellarValue const&
    getValue() const
    {
        return mValue;
    }
    optional<Hash> const&
    getExpectedHash() const
    {
        return mExpectedLedgerHash;
    }

  private:
    uint32_t mLedgerSeq;
    std::shared_ptr<AbstractTxSetFrameForApply> mTxSet;
    StellarValue mValue;
    optional<Hash> mExpectedLedgerHash;
};

std::string stellarValueToString(Config const& c, StellarValue const& sv);

#define emptyUpgradeSteps (xdr::xvector<UpgradeType, 6>(0))
}
