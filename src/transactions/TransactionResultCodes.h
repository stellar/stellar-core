#ifndef __TRANSACTIONRESULTCODES__
#define __TRANSACTIONRESULTCODES__
namespace stellar
{

    enum TxResultCode
    {
        txSUCCESS,
        txALREADY,
        txNOFEE,
        txMALFORMED
    };
}

#endif

