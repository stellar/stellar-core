#ifndef __TXRESULT__
#define __TXRESULT__
/*
This is the result of one transaction being applied to the ledger
*/
class TxResult
{
    stellarxdr::uint256 mTxID;
    TxResultCode mCode;
public:
    void setID(stellarxdr::uint256& id);
    void setCode(TxResultCode code);
};

#endif