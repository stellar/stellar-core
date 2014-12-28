// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TransactionFrame.h"
#include "xdrpp/marshal.h"
#include <string>
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"

namespace stellar
{

    TransactionFrame::pointer TransactionFrame::makeTransactionFromWire(TransactionEnvelope const& msg)
{
    //mSignature = msg.signature;

	// SANITY check sig
    return TransactionFrame::pointer();
}

// called when determining if we should accept this tx.
// check for the malformed, correct sig, min fee
bool TransactionFrame::isValid()
{
    // TODO.2


    return true;
}

// TODO.2 we can probably get rid of this
uint256& TransactionFrame::getSignature()
{
    return mEnvelope.signature;
}

uint256& TransactionFrame::getHash()
{
	if(isZero(mHash))
	{
        Transaction tx;
        toXDR(tx);
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(tx));
        hashXDR(std::move(xdrBytes), mHash);
	}
	return(mHash);
}


// TODO.2 factor in the transfer rate
// convert from sheep to wheat
// doesn't check limits assumes that the account has the amountToSell
// returns the amount of sheep Sold
int64_t TransactionFrame::convert(Currency& sheep,
    Currency& wheat, int64_t amountOfSheepToSell, int64_t minSheepPrice,
    TxDelta& delta, LedgerMaster& ledgerMaster)
{   
    int64_t maxWheatPrice = (OFFER_PRICE_DIVISOR*OFFER_PRICE_DIVISOR) / minSheepPrice;

    int64_t amountOfSheepSold = 0;
    int offerOffset = 0;
    while(amountOfSheepToSell > 0)
    {
        vector<OfferFrame> retList;
        ledgerMaster.getDatabase().loadBestOffers(5, offerOffset, wheat, sheep, retList);
        for(auto wheatOffer : retList)
        {
            if(wheatOffer.mEntry.offer().price > maxWheatPrice)
            { // wheat is too high!
                return amountOfSheepSold;
            }

            
            int64_t amountOfWheatOwned = ledgerMaster.getDatabase().getOfferAmountFunded(wheatOffer);

            delta.setStart(wheatOffer);
            int64_t canFill = (amountOfWheatOwned*wheatOffer.mEntry.offer().price) / OFFER_PRICE_DIVISOR;
            if(canFill > amountOfSheepToSell)
            { // need to only take part of the offer
                int64_t amountToTake = amountOfSheepToSell*OFFER_PRICE_DIVISOR / wheatOffer.mEntry.offer().price;
                wheatOffer.mEntry.offer().amount -= amountToTake;
                if(wheatOffer.mEntry.offer().amount < 0)
                {
                    CLOG(ERROR, "Tx") << "Transaction::convert offer amount below 0";
                } else delta.setFinal(wheatOffer);
                
                TrustFrame newSheepOwnerLine;
                if(!ledgerMaster.getDatabase().loadTrustLine(wheatOffer.mEntry.offer().accountID, sheep.ci(), newSheepOwnerLine))
                {
                    CLOG(ERROR, "Tx") << "Buyer trustline not found?";
                    mResultCode = txINTERNAL_ERROR;
                    return 0;
                }
                delta.setStart(newSheepOwnerLine);

                amountOfSheepSold += amountOfSheepToSell;
                amountOfSheepToSell = 0;
                break;
            } else
            {   // take the whole offer                
                amountOfSheepToSell -= canFill;

                // offer will be deleted since we don't add a delta.setFinal for it
            }

        }
        // still stuff to fill but no more offers
        if(retList.size() < 5) return amountOfSheepSold;
        offerOffset += retList.size();
    }

    return amountOfSheepSold;
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

void TransactionFrame::apply(TxDelta& delta, LedgerMaster& ledgerMaster)
{
    if(ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.account, mSigningAccount))
    {
        if(preApply(delta,ledgerMaster))
        {
            doApply(delta,ledgerMaster);
        }
    } else
    {
        CLOG(ERROR, "Tx") << "Signing account not found. This should never happen";
    }
    
}



void TransactionFrame::toXDR(Transaction& envelope)
{
    // LATER
}

void TransactionFrame::toXDR(TransactionEnvelope& envelope)
{
    // LATER
}

StellarMessage&& TransactionFrame::toStellarMessage()
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    toXDR(msg.transaction());
    return std::move(msg);
}
}
