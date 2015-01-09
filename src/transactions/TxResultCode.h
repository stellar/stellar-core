#ifndef __TRANSACTIONRESULTCODES__
#define __TRANSACTIONRESULTCODES__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace stellar
{
    // TODO: should be moved to .x file as it's protocol definition

    enum TxResultCode
    {
        txSUCCESS = 0,
        txBAD_AUTH,
        txBAD_SEQ, // txPRE_SEQ, txPAST_SEQ,
        txBAD_LEDGER, // txMIN_LEDGER,txMAX_LEDGER,
        txINSUFFICIENT_FEE,
        txMALFORMED,
        txALREADY,
        txNOFEE,
        txUNDERFUNDED,
        txBADRATE,
        txLINEFULL,
        txOVERSENDMAX,
        txNOTRUST,
        txOFFER_NOT_FOUND,
        txRATE_FIXED,
        txNOT_AUTHORIZED,
        txINTERNAL_ERROR,
        txCROSS_SELF,
        txCREDIT_ISSUED,
        txBELOW_MIN_BALANCE,


        txNOACCOUNT,
        txUNKNOWN
    };
}

#endif

