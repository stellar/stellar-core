// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TransactionFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
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
#include <cereal/external/base64.hpp>

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

    return TransactionFrame::pointer();
}

TransactionFrame::TransactionFrame(const TransactionEnvelope& envelope) : mEnvelope(envelope)
{
}
    
Hash& TransactionFrame::getFullHash()
{
    if(isZero(mFullHash))
    {
        mFullHash = sha256(xdr::xdr_to_msg(mEnvelope));
    }
    return(mFullHash);
}

Hash& TransactionFrame::getContentsHash()
{
    if(isZero(mContentsHash))
    {
        mContentsHash = sha256(xdr::xdr_to_msg(mEnvelope.tx));
	}
	return(mContentsHash);
}


TransactionEnvelope& TransactionFrame::getEnvelope()
{
    return mEnvelope;
}

void TransactionFrame::setSourceAccountPtr(AccountFrame::pointer signingAccount)
{
    if (!signingAccount)
    {
        if (mEnvelope.tx.account != signingAccount->getID())
        {
            throw std::invalid_argument("wrong account");
        }
    }
    mSigningAccount = signingAccount;
}

bool TransactionFrame::preApply(LedgerDelta& delta,LedgerMaster& ledgerMaster)
{
    Database &db = ledgerMaster.getDatabase();
    int32_t fee = ledgerMaster.getTxFee();

    if (mSigningAccount->getAccount().balance < fee)
    {
        mResult.body.code(txNO_FEE);

        // take all their balance to be safe
        mSigningAccount->getAccount().balance = 0;
        mSigningAccount->storeChange(delta, db);
        return false;
    }
    mSigningAccount->setSeqSlot(mEnvelope.tx.seqSlot, mEnvelope.tx.seqNum);
    mSigningAccount->getAccount().balance -= fee;
    mResult.feeCharged = fee;
    ledgerMaster.getCurrentLedgerHeader().feePool += fee;

    mSigningAccount->storeChange(delta, db);

    return true;
}

bool TransactionFrame::apply(LedgerDelta& delta, Application& app)
{
    bool res;

    mResult.body.code(txINTERNAL_ERROR);

    if(checkValid(app))
    {
        // this can't be done in checkValid since we should still flood txs 
        // where seq != envelope.seq
        if(mSigningAccount->getSeq(mEnvelope.tx.seqSlot, app.getDatabase())+1 != mEnvelope.tx.seqNum)
        {
            mResult.body.code(txBAD_SEQ);
            return true;  // needs to return true since it will still claim a fee
        }

        res = true;

        LedgerMaster &lm = app.getLedgerMaster();

        bool pre_res = preApply(delta, lm);

        if (pre_res)
        {
            soci::transaction sqlTx(lm.getDatabase().getSession());
            LedgerDelta txDelta(delta.getCurrentID());

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
    if(mSigningAccount->getAccount().thresholds[0])
        keyWeights.push_back(Signer(mSigningAccount->getID(),mSigningAccount->getAccount().thresholds[0]));

    keyWeights.insert(keyWeights.end(), mSigningAccount->getAccount().signers.begin(), mSigningAccount->getAccount().signers.end());

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
                if(totalWeight >= getNeededThreshold())
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
    // OPTIMIZE: we could cache the AccountFrames so we don't have to 
    //   keep looking them up for every tx
    AccountFrame::pointer account = make_shared<AccountFrame>();
    res = AccountFrame::loadAccount(mEnvelope.tx.account,
        *account, app.getDatabase(), true);
    if (res)
    {
        mSigningAccount = account;
    } else mSigningAccount = AccountFrame::pointer();
   
    return res;
}

TransactionResultCode TransactionFrame::getResultCode()
{
    return mResult.body.code();
}

// called when determining if we should accept this tx.
// called when determining if we should flood
// make sure sig is correct
// make sure maxFee is above the current fee
// make sure it is in the correct ledger bounds
// don't consider minBalance since you want to allow them to still send
// around credit etc
bool TransactionFrame::checkValid(Application& app)
{
    int32_t fee = app.getLedgerGateway().getTxFee();

    if (mEnvelope.tx.maxFee < app.getLedgerGateway().getTxFee())
    {
        mResult.body.code(txINSUFFICIENT_FEE);
        return false;
    }

    if(mEnvelope.tx.maxLedger < app.getLedgerGateway().getLedgerNum())
    {
        mResult.body.code(txBAD_LEDGER);
        return false;
    }
    if(mEnvelope.tx.minLedger > app.getLedgerGateway().getLedgerNum())
    {
        mResult.body.code(txBAD_LEDGER);
        return false;
    }

    if (!loadAccount(app))
    {
        mResult.body.code(txNO_ACCOUNT);
        return false;
    }

    if(mSigningAccount->getSeq(mEnvelope.tx.seqSlot,app.getDatabase())>=mEnvelope.tx.seqNum)
    {
        mResult.body.code(txBAD_SEQ);
        return false;
    }

    if (!checkSignature())
    {
        mResult.body.code(txBAD_AUTH);
        return false;
    }

    if (mSigningAccount->getAccount().balance < fee)
    {
        mResult.body.code(txNO_FEE);
        return false;
    }

    mResult.body.code(txINNER);
    mResult.body.tr().type(
        mEnvelope.tx.body.type());

    return doCheckValid(app);
}

StellarMessage TransactionFrame::toStellarMessage()
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction()=mEnvelope;
    return msg;
}

void TransactionFrame::storeTransaction(LedgerMaster &ledgerMaster, LedgerDelta const& delta)
{
    xdr::msg_ptr txBytes(xdr::xdr_to_msg(mEnvelope));
    xdr::msg_ptr txResultBytes(xdr::xdr_to_msg(mResult));

    std::string txBody = base64::encode(
        reinterpret_cast<const unsigned char *>(txBytes->raw_data()),
        txBytes->raw_size());

    std::string txResult = base64::encode(
        reinterpret_cast<const unsigned char *>(txResultBytes->raw_data()),
        txResultBytes->raw_size());

    xdr::msg_ptr txMeta(delta.getTransactionMeta());

    std::string meta = base64::encode(
        reinterpret_cast<const unsigned char *>(txMeta->raw_data()),
        txMeta->raw_size());

    string txIDString(binToHex(getContentsHash()));

    auto timer = ledgerMaster.getDatabase().getInsertTimer("txhistory");
    soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
        "INSERT INTO TxHistory (txID, ledgerSeq, TxBody, TxResult, TxMeta) VALUES "\
        "(:id,:seq,:txb,:txres,:entries)",
        soci::use(txIDString), soci::use(ledgerMaster.getCurrentLedgerHeader().ledgerSeq),
        soci::use(txBody), soci::use(txResult), soci::use(meta));

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
        "CREATE TABLE TxHistory ("
        "txID          CHARACTER(64) NOT NULL,"
        "ledgerSeq     INT NOT NULL CHECK (ledgerSeq >= 0),"
        "TxBody        TEXT NOT NULL,"
        "TxResult      TEXT NOT NULL,"
        "TxMeta        TEXT NOT NULL,"
        "PRIMARY KEY (txID, ledgerSeq)"
        ")";

}

}
