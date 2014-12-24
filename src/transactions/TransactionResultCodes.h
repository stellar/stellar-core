#ifndef __TRANSACTIONRESULTCODES__
#define __TRANSACTIONRESULTCODES__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{

    enum TxResultCode
    {
        txSUCCESS,
        txALREADY,
        txNOFEE,
        txMALFORMED,
        txBADSEQ,
        txUNDERFUNDED,
        txBADRATE,
        txLINEFULL,
        txOVERSENDMAX,
        txNOTRUST,


        txNOACCOUNT
    };
}

#endif

