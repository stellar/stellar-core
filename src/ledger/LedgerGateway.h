#ifndef __LEDGERGATEWAY__
#define __LEDGERGATEWAY__

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
    virtual void externalizeValue(TxSetFramePtr txSet)=0;

    virtual int64_t getTxFee() = 0;
    virtual uint64_t getLedgerNum() = 0;
};
}

#endif
