// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TransactionFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "transactions/AllowTrustTxFrame.h"
#include "transactions/CancelOfferFrame.h"
#include "transactions/CreateOfferFrame.h"
#include "transactions/ChangeTrustTxFrame.h"
#include "transactions/InflationFrame.h"
#include "transactions/MergeFrame.h"
#include "transactions/PaymentFrame.h"
#include "transactions/SetOptionsFrame.h"
#include "database/Database.h"

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
    

uint256& TransactionFrame::getID()
{
    if(isZero(mID))
    {
        mID = sha512_256(xdr::xdr_to_msg(mEnvelope));
    }
    return(mID);
}

Hash& TransactionFrame::getHash()
{
	if(isZero(mHash))
    {
        mHash = sha512_256(xdr::xdr_to_msg(mEnvelope.tx));
	}
	return(mHash);
}

TransactionEnvelope&
TransactionFrame::getEnvelope()
{
    return mEnvelope;
}


int64_t TransactionFrame::getTransferRate(Currency& currency, LedgerMaster& ledgerMaster)
{
    if(currency.type() == NATIVE) return TRANSFER_RATE_DIVISOR;

    AccountFrame issuer;
    
	if (!ledgerMaster.getDatabase().loadAccount(currency.isoCI().issuer, issuer))
    {
        throw std::runtime_error("Account not found in TransactionFrame::getTransferRate");
    }
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
        mResultCode = txBAD_SEQ;
        CLOG(ERROR, "Tx") << "Tx sequence # doesn't match Account in validated set. This should never happen.";
        return false;
    }

    if((ledgerMaster.getCurrentLedgerHeader().ledgerSeq > mEnvelope.tx.maxLedger) ||
        (ledgerMaster.getCurrentLedgerHeader().ledgerSeq < mEnvelope.tx.minLedger))
    {
        mResultCode = txBAD_LEDGER;
        CLOG(ERROR, "Tx") << "tx not in valid ledger in validated set. This should never happen.";
        return false;
    }
        
    uint32_t fee=ledgerMaster.getTxFee();
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

        // take all their balance to be safe
        delta.addFee(mSigningAccount.mEntry.account().balance);
        mSigningAccount.mEntry.account().balance = 0;
        delta.setFinal(mSigningAccount);
        return false;
    }

    delta.setStart(mSigningAccount);

    mSigningAccount.mEntry.account().balance -= fee;
    mSigningAccount.mEntry.account().sequence += 1;
    delta.addFee(fee);
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

void TransactionFrame::addSignature(const SecretKey& secretKey)
{
    uint512 sig = secretKey.sign(getHash());
    mEnvelope.signatures.push_back(sig);
}


int32_t TransactionFrame::getNeededThreshold()
{
    return mSigningAccount.getMidThreshold();
}

// TODO.2 make sure accounts default with threshold of master key to 1
bool TransactionFrame::checkSignature()
{
    vector<Signer> keyWeights;
    if(mSigningAccount.mEntry.account().thresholds[0])
        keyWeights.push_back(Signer(mSigningAccount.getID(),mSigningAccount.mEntry.account().thresholds[0]));

    keyWeights.insert(keyWeights.end(), mSigningAccount.mEntry.account().signers.begin(), mSigningAccount.mEntry.account().signers.end());
   
    
    // make sure not too many signatures attached to the tx
    if(keyWeights.size() < mEnvelope.signatures.size())
        return false;

    Hash& txHash = getHash();
    
    // calculate the weight of the signatures
    int totalWeight = 0;
    for(auto sig : mEnvelope.signatures)
    {
        bool found = false;
        for(auto it = keyWeights.begin(); it != keyWeights.end(); it++)
        {
            if(PublicKey::verifySig((*it).pubKey, sig, txHash))
            {
                totalWeight += (*it).weight;
                if(totalWeight > getNeededThreshold())
                    return true;

                keyWeights.erase(it);  // can't sign twice
                found = true;
                break;
            }
        }
        if(!found) return false;  // some random person signed it
    }

    return false;
}

bool TransactionFrame::loadAccount(Application& app)
{
    return app.getDatabase().loadAccount(mEnvelope.tx.account, mSigningAccount, true);
}

// called when determining if we should accept this tx.
// make sure sig is correct
// make sure maxFee is above the current fee
// make sure it is in the correct ledger bounds
bool TransactionFrame::checkValid(Application& app)
{
    if (mEnvelope.tx.maxFee < app.getLedgerGateway().getTxFee())
    {
        mResultCode = txINSUFFICIENT_FEE;
        return false;
    }
    if (mEnvelope.tx.maxLedger < app.getLedgerGateway().getLedgerNum())
    {
        mResultCode = txBAD_LEDGER;
        return false;
    }
    if (mEnvelope.tx.minLedger > app.getLedgerGateway().getLedgerNum())
    {
        mResultCode = txBAD_LEDGER;
        return false;
    }
    if (!loadAccount(app))
    {
        mResultCode = txNOACCOUNT;
        return false;
    }
    if (!checkSignature())
    {
        mResultCode = txBAD_AUTH;
        return false;
    }

    return doCheckValid(app);
}

StellarMessage&& TransactionFrame::toStellarMessage()
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction()=mEnvelope;
    return std::move(msg);
}
}
