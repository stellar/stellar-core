// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "TransactionFrame.h"
#include "xdrpp/marshal.h"
#include <string>
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "crypto/SHA.h"
#include "transactions/AllowTrustTxFrame.h"
#include "transactions/CancelOfferFrame.h"
#include "transactions/CreateOfferFrame.h"
#include "transactions/ChangeTrustTxFrame.h"
#include "transactions/InflationFrame.h"
#include "transactions/MergeFrame.h"
#include "transactions/PaymentFrame.h"
#include "transactions/SetOptionsFrame.h"


namespace stellar
{

   
TransactionFrame::pointer TransactionFrame::makeTransactionFromWire(TransactionEnvelope const& msg)
{
    switch(msg.tx.body.type())
    {
    case PAYMENT:
        return TransactionFrame::pointer(new PaymentFrame(msg));
    case CREATE_OFFER:
        return TransactionFrame::pointer(new CreateOfferFrame(msg));
    case CANCEL_OFFER:
        return TransactionFrame::pointer(new CancelOfferFrame(msg));
    case SET_OPTIONS:
        return TransactionFrame::pointer(new SetOptionsFrame(msg));
    case CHANGE_TRUST:
        return TransactionFrame::pointer(new ChangeTrustTxFrame(msg));
    case ALLOW_TRUST:
        return TransactionFrame::pointer(new AllowTrustTxFrame(msg));
    case ACCOUNT_MERGE:
        return TransactionFrame::pointer(new MergeFrame(msg));
    case INFLATION:
        return TransactionFrame::pointer(new InflationFrame(msg));

    default:
        CLOG(WARNING, "Tx") << "Unknown Tx type: " << msg.tx.body.type();
    }
    //mSignature = msg.signature;

	// SANITY check sig
    return TransactionFrame::pointer();
}

TransactionFrame::TransactionFrame(const TransactionEnvelope& envelope) : mEnvelope(envelope)
{
    mResultCode = txUNKNOWN;
}
    
uint512& TransactionFrame::getSignature()
{
    return mEnvelope.signatures[0];  // TODO.2 do we need this function?
}

uint256& TransactionFrame::getHash()
{
	if(isZero(mHash))
    {
        mHash = sha512_256(xdr::xdr_to_msg(mEnvelope.tx));
	}
	return(mHash);
}


int64_t TransactionFrame::getTransferRate(Currency& currency, LedgerMaster& ledgerMaster)
{
    if(currency.native()) return TRANSFER_RATE_DIVISOR;

    AccountFrame issuer;
    ledgerMaster.getDatabase().loadAccount(currency.ci().issuer, issuer);
    return issuer.mEntry.account().transferRate;    
}



/*
// returns true if this account can hold this currency
bool Transaction::isAuthorizedToHold(const AccountEntry& account, 
    const CurrencyIssuer& ci, LedgerMaster& ledgerMaster)
{
    LedgerEntry issuer;
    if(ledgerMaster.getDatabase().loadAccount(ci.issuer,issuer))
    {
        if(issuer.account().flags && AccountEntry::AUTH_REQUIRED_FLAG)
        {

        } else return true;
    } else return false;
}
*/
    

// take fee
// check seq
// take seq
// check max ledger
bool TransactionFrame::preApply(TxDelta& delta,LedgerMaster& ledgerMaster)
{
    if(mSigningAccount.mEntry.account().sequence != mEnvelope.tx.seqNum)
    {
        mResultCode = txBADSEQ;
        CLOG(ERROR, "Tx") << "Tx sequence # doesn't match Account in validated set. This should never happen.";
        return false;
    }

    if((ledgerMaster.getCurrentLedger()->mHeader.ledgerSeq > mEnvelope.tx.maxLedger) ||
        (ledgerMaster.getCurrentLedger()->mHeader.ledgerSeq < mEnvelope.tx.minLedger))
    {
        mResultCode = txMALFORMED;
        CLOG(ERROR, "Tx") << "tx not in valid ledger in validated set. This should never happen.";
        return false;
    }
        
    uint32_t fee=ledgerMaster.getCurrentLedger()->getTxFee();
    if(fee > mEnvelope.tx.maxFee)
    {
        mResultCode = txNOFEE;
        CLOG(ERROR, "Tx") << "tx isn't willing to pay fee. This should never happen.";
        return false;
    }


    if(mSigningAccount.mEntry.account().balance < fee)
    {
        mResultCode = txNOFEE;
        CLOG(ERROR, "Tx") << "tx doesn't have fee. This should never happen.";
        // TODO.2: what do we do here? take all their balance?
        return false;
    }

    delta.setStart(mSigningAccount);

    mSigningAccount.mEntry.account().balance -= fee;
    mSigningAccount.mEntry.account().sequence += 1;

    delta.setFinal(mSigningAccount);
    return true;
}

void TransactionFrame::apply(TxDelta& delta, Application& app)
{
    if(checkValid(app))
    {
        if(preApply(delta,app.getLedgerMaster()))
        {
            doApply(delta,app.getLedgerMaster());
        }
    } else
    {
        CLOG(ERROR, "Tx") << "invalid tx. This should never happen";
    }
    
}


int32_t TransactionFrame::getNeededThreshold()
{
    //mSigningAccount.mEntry.account().thresholds;
    return 0; // TODO.2
}

bool TransactionFrame::checkSignature()
{
    // TODO.2
    // calculate the weight of the signatures
    int totalWeight = 0;
    if(totalWeight>getNeededThreshold())
        return true;
    return false;
}

// called when determining if we should accept this tx.
// make sure sig is correct
// make sure maxFee is above the current fee
// make sure it is in the correct ledger bounds
bool TransactionFrame::checkValid(Application& app)
{
    if(mEnvelope.tx.maxFee < app.getLedgerGateway().getFee()) return false;
    if(mEnvelope.tx.maxLedger < app.getLedgerGateway().getLedgerNum()) return false;
    if(mEnvelope.tx.minLedger > app.getLedgerGateway().getLedgerNum()) return false;
    if(!app.getDatabase().loadAccount(mEnvelope.tx.account, mSigningAccount, true)) return false;
    if(!checkSignature()) return false;

    return doCheckValid(app);
}

void TransactionFrame::toXDR(Transaction& envelope)
{
    // TODO.2
}

void TransactionFrame::toXDR(TransactionEnvelope& envelope)
{
    // TODO.2
}

StellarMessage&& TransactionFrame::toStellarMessage()
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    toXDR(msg.transaction());
    return std::move(msg);
}
}
