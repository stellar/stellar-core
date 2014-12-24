// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Transaction.h"
#include "xdrpp/marshal.h"
#include <string>
#include "lib/json/json.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"

namespace stellar
{

Transaction::pointer Transaction::makeTransactionFromWire(stellarxdr::TransactionEnvelope const& msg)
{
    //mSignature = msg.signature;

	// SANITY check sig
    return Transaction::pointer();
}

// called when determining if we should accept this tx.
// check for the malformed, correct sig, min fee
bool Transaction::isValid()
{
    // TODO.2


    return true;
}

stellarxdr::uint256& Transaction::getSignature()
{
    return mSignature;
}

stellarxdr::uint256& Transaction::getHash()
{
	if(isZero(mHash))
	{
        stellarxdr::Transaction tx;
        toXDR(tx);
        xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(tx));
        hashXDR(std::move(xdrBytes), mHash);
	}
	return(mHash);
}

    

// take fee
// check seq
// take seq
// check max ledger
bool Transaction::preApply(TxDelta& delta,LedgerMaster& ledgerMaster)
{
    if(mSigningAccount->mEntry.account().sequence != mEnvelope.tx.seqNum)
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


    if(mSigningAccount->mEntry.account().balance < fee)
    {
        mResultCode = txNOFEE;
        CLOG(ERROR, "Tx") << "tx doesn't have fee. This should never happen.";
        // TODO.2: what do we do here? take all their balance?
        return false;
    }

    delta.setStart(*mSigningAccount.get());

    mSigningAccount->mEntry.account().balance -= fee;
    mSigningAccount->mEntry.account().sequence += 1;

    delta.setFinal(*mSigningAccount.get());
    return true;
}

void Transaction::apply(TxDelta& delta, LedgerMaster& ledgerMaster)
{
    stellarxdr::LedgerEntry entry;
    if(ledgerMaster.getDatabase().loadAccount(mEnvelope.tx.account, entry))
    {
        mSigningAccount=std::make_shared<AccountEntry>(entry);
        if(preApply(delta,ledgerMaster))
        {
            doApply(delta,ledgerMaster);
        }
    } else
    {
        CLOG(ERROR, "Tx") << "Signing account not found. This should never happen";
    }
    
}

bool Transaction::isNativeCurrency(stellarxdr::Currency currency)
{
    return(currency.size() == 0);
}

void Transaction::toXDR(stellarxdr::Transaction& envelope)
{
    // LATER
}

void Transaction::toXDR(stellarxdr::TransactionEnvelope& envelope)
{
    // LATER
}

stellarxdr::StellarMessage&& Transaction::toStellarMessage()
{
    stellarxdr::StellarMessage msg;
    msg.type(stellarxdr::TRANSACTION);
    toXDR(msg.transaction());
    return std::move(msg);
}
}
