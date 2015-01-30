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
#include "crypto/Hex.h"


namespace stellar
{

using namespace std;
   
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
}
    
Hash& TransactionFrame::getFullHash()
{
    if(isZero(mFullHash))
    {
        mFullHash = sha512_256(xdr::xdr_to_msg(mEnvelope));
    }
    return(mFullHash);
}

Hash& TransactionFrame::getContentsHash()
{
    if(isZero(mContentsHash))
    {
        mContentsHash = sha512_256(xdr::xdr_to_msg(mEnvelope.tx));
	}
	return(mContentsHash);
}


TransactionEnvelope&
TransactionFrame::getEnvelope()
{
    return mEnvelope;
}


int64_t TransactionFrame::getTransferRate(const Currency& currency, LedgerMaster& ledgerMaster)
{
    if (currency.type() == NATIVE)
    {
        return TRANSFER_RATE_DIVISOR;
    }

    AccountFrame issuer;
    if (!AccountFrame::loadAccount(currency.isoCI().issuer, issuer, ledgerMaster.getDatabase()))
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
    

bool TransactionFrame::preApply(LedgerDelta& delta,LedgerMaster& ledgerMaster)
{

    Database &db = ledgerMaster.getDatabase();
    int32_t fee = ledgerMaster.getTxFee();

    if (mSigningAccount->mEntry.account().balance < fee)
    {
        mResult.body.code(txNO_FEE);

        // take all their balance to be safe
        mSigningAccount->mEntry.account().balance = 0;
        mSigningAccount->storeChange(delta, db);
        return false;
    }

    mSigningAccount->mEntry.account().balance -= fee;
    mSigningAccount->mEntry.account().sequence += 1;
    mResult.feeCharged = fee;
    ledgerMaster.getCurrentLedgerHeader().feePool += fee;

    mSigningAccount->storeChange(delta, db);

    return true;
}

bool TransactionFrame::apply(LedgerDelta& delta, Application& app)
{
    bool res;

    if(checkValid(app))
    {
        res = true;

        LedgerMaster &lm = app.getLedgerMaster();

        bool pre_res = preApply(delta, lm);

        if (pre_res)
        {
            soci::transaction sqlTx(lm.getDatabase().getSession());
            LedgerDelta txDelta;

            bool apply_res = doApply(txDelta, lm);
            if (apply_res)
            {
                sqlTx.commit();
                delta.merge(txDelta);
            }
        }
    }
    else
    {
        res = false;
    }

    return res;
}

void TransactionFrame::addSignature(const SecretKey& secretKey)
{
    uint512 sig = secretKey.sign(getContentsHash());
    mEnvelope.signatures.push_back(sig);
}


int32_t TransactionFrame::getNeededThreshold()
{
    return mSigningAccount->getMidThreshold();
}

bool TransactionFrame::checkSignature()
{
    vector<Signer> keyWeights;
    if(mSigningAccount->mEntry.account().thresholds[0])
        keyWeights.push_back(Signer(mSigningAccount->getID(),mSigningAccount->mEntry.account().thresholds[0]));

    keyWeights.insert(keyWeights.end(), mSigningAccount->mEntry.account().signers.begin(), mSigningAccount->mEntry.account().signers.end());

    // make sure not too many signatures attached to the tx
    if(keyWeights.size() < mEnvelope.signatures.size())
        return false;

    getContentsHash();

    // calculate the weight of the signatures
    int totalWeight = 0;
    for(auto sig : mEnvelope.signatures)
    {
        bool found = false;
        for(auto it = keyWeights.begin(); it != keyWeights.end(); it++)
        {
            if(PublicKey::verifySig((*it).pubKey, sig, mContentsHash))
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
    bool res;
    if (!mSigningAccount)
    {
        AccountFrame::pointer account = make_shared<AccountFrame>();
        res = AccountFrame::loadAccount(mEnvelope.tx.account,
            *account, app.getDatabase(), true);

        if (res)
        {
            mSigningAccount = account;
        }
    }
    else
    {
        res = true;
    }
    return res;
}

TransactionResultCode TransactionFrame::getResultCode()
{
    return mResult.body.code();
}

// called when determining if we should accept this tx.
// make sure sig is correct
// make sure maxFee is above the current fee
// make sure it is in the correct ledger bounds
bool TransactionFrame::checkValid(Application& app)
{
    if (mEnvelope.tx.maxFee < app.getLedgerGateway().getTxFee())
    {
        mResult.body.code(txINSUFFICIENT_FEE);
        return false;
    }
    if (mEnvelope.tx.maxLedger < app.getLedgerGateway().getLedgerNum())
    {
        mResult.body.code(txBAD_LEDGER);
        return false;
    }
    if (mEnvelope.tx.minLedger > app.getLedgerGateway().getLedgerNum())
    {
        mResult.body.code(txBAD_LEDGER);
        return false;
    }

    int32_t fee = app.getLedgerGateway().getTxFee();

    if (fee > mEnvelope.tx.maxFee)
    {
        mResult.body.code(txNO_FEE);
        return false;
    }

    if (!loadAccount(app))
    {
        mResult.body.code(txNO_ACCOUNT);
        return false;
    }

    if (!checkSignature())
    {
        mResult.body.code(txBAD_AUTH);
        return false;
    }

    if (mSigningAccount->mEntry.account().sequence != mEnvelope.tx.seqNum)
    {
        mResult.body.code(txBAD_SEQ);
        return false;
    }

    if (mSigningAccount->mEntry.account().balance < fee)
    {
        mResult.body.code(txNO_FEE);
        return false;
    }

    mResult.body.code(txINNER);
    mResult.body.tr().type(
        mEnvelope.tx.body.type());

    return doCheckValid(app);
}

StellarMessage&& TransactionFrame::toStellarMessage()
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction()=mEnvelope;
    return std::move(msg);
}

void TransactionFrame::storeTransaction(LedgerMaster &ledgerMaster)
{
    soci::blob txBlob(ledgerMaster.getDatabase().getSession());
    soci::blob txResultBlob(ledgerMaster.getDatabase().getSession());

    xdr::msg_ptr txBytes(xdr::xdr_to_msg(mEnvelope));
    xdr::msg_ptr txResultBytes(xdr::xdr_to_msg(mResult));

    txBlob.write(0, txBytes->raw_data(), txBytes->raw_size());
    txResultBlob.write(0, txResultBytes->raw_data(), txResultBytes->raw_size());

    string txIDString(binToHex(getContentsHash()));

    soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
        "INSERT INTO TxHistory (txID, ledgerSeq, Tx, TxResult) VALUES "\
        "(:id,:seq,:tx,:txres)",
        soci::use(txIDString), soci::use(ledgerMaster.getCurrentLedgerHeader().ledgerSeq),
        soci::use(txBlob), soci::use(txResultBlob));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void TransactionFrame::dropAll(Database &db)
{
    db.getSession() << "DROP TABLE IF EXISTS TxHistory";

    db.getSession() <<
        "CREATE TABLE IF NOT EXISTS TxHistory (" \
        "txID       CHARACTER(35) NOT NULL,"\
        "ledgerSeq  INT UNSIGNED NOT NULL,"\
        "Tx         BLOB NOT NULL,"\
        "TxResult   BLOB NOT NULL,"\
        "PRIMARY KEY (txID, ledgerSeq)"\
        ")";

}

}
