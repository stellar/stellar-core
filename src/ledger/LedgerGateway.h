#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "herder/TxSetFrame.h"

namespace stellar
{
class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;

/*
 * Public Interface to the Ledger Module
 */
class LedgerGateway
{
  public:
    // called by txherder
    virtual void externalizeValue(TxSetFramePtr txSet, uint64_t closeTime, int32_t baseFee)=0;

    virtual uint64_t getLedgerNum() = 0;
    virtual uint64_t getCloseTime() = 0;
    virtual int32_t getTxFee() = 0;
    virtual bool hasTxBeenApplied(Hash& hash) = 0;
};
}


