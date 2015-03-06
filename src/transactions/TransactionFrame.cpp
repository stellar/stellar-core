// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TransactionFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "util/Logging.h"
#include "util/XDRStream.h"
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

shared_ptr<OperationFrame> OperationFrame::makeHelper(Operation const& op,
    OperationResult &res, TransactionFrame &tx)
{
    switch (op.body.type())
    {
    case PAYMENT:
        return shared_ptr<OperationFrame>(new PaymentFrame(op, res, tx));
    case CREATE_OFFER:
        return shared_ptr<OperationFrame>(new CreateOfferFrame(op, res, tx));
    case CANCEL_OFFER:
        return shared_ptr<OperationFrame>(new CancelOfferFrame(op, res, tx));
    case SET_OPTIONS:
        return shared_ptr<OperationFrame>(new SetOptionsFrame(op, res, tx));
    case CHANGE_TRUST:
        return shared_ptr<OperationFrame>(new ChangeTrustTxFrame(op, res, tx));
    case ALLOW_TRUST:
        return shared_ptr<OperationFrame>(new AllowTrustTxFrame(op, res, tx));
    case ACCOUNT_MERGE:
        return shared_ptr<OperationFrame>(new MergeFrame(op, res, tx));
    case INFLATION:
        return shared_ptr<OperationFrame>(new InflationFrame(op, res, tx));

    default:
        ostringstream err;
        err << "Unknown Tx type: " << op.body.type();
        throw std::invalid_argument(err.str());
    }
}

TransactionFrame::pointer TransactionFrame::makeTransactionFromWire(TransactionEnvelope const& msg)
{
    TransactionFrame::pointer res = make_shared<TransactionFrame>(msg);
    return res;
}


OperationFrame::OperationFrame(Operation const& op, OperationResult &res, TransactionFrame & parentTx)
    : mOperation(op), mParentTx(parentTx), mResult(res)
{
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

bool OperationFrame::apply(LedgerDelta& delta, Application& app)
{
    bool res;
    res = checkValid(app);
    if(res)
    {
        res = doApply(delta, app.getLedgerMaster());
    }

    return res;
}

void TransactionFrame::addSignature(const SecretKey& secretKey)
{
    uint512 sig = secretKey.sign(getContentsHash());
    mEnvelope.signatures.push_back(sig);
}


int32_t OperationFrame::getNeededThreshold()
{
    return mSourceAccount->getMidThreshold();
}

bool OperationFrame::checkSignature()
{
    return mParentTx.checkSignature(*mSourceAccount, getNeededThreshold());
}

bool TransactionFrame::checkSignature(AccountFrame& account, int32_t neededWeight)
{
    vector<Signer> keyWeights;
    if(account.getAccount().thresholds[0])
        keyWeights.push_back(Signer(account.getID(), account.getAccount().thresholds[0]));

    keyWeights.insert(keyWeights.end(), account.getAccount().signers.begin(), account.getAccount().signers.end());

    Hash const& contentsHash = getContentsHash();

    // calculate the weight of the signatures
    int totalWeight = 0;
    for(auto sig : getEnvelope().signatures)
    {
        for(auto it = keyWeights.begin(); it != keyWeights.end(); it++)
        {
            if(PublicKey::verifySig((*it).pubKey, sig, contentsHash))
            {
                totalWeight += (*it).weight;
                if(totalWeight >= neededWeight)
                    return true;

                keyWeights.erase(it);  // can't sign twice
                break;
            }
        }
    }

    return false;
}

uint256 const& OperationFrame::getSourceID()
{
    return mOperation.sourceAccount ?
        *mOperation.sourceAccount : mParentTx.getEnvelope().tx.account;
}

bool OperationFrame::loadAccount(Application& app)
{
    mSourceAccount = mParentTx.loadAccount(app, getSourceID());
    return !!mSourceAccount;
}

AccountFrame::pointer TransactionFrame::loadAccount(Application& app, uint256 const& accountID)
{
    AccountFrame::pointer res;

    if (mSigningAccount && mSigningAccount->getID() == accountID)
    {
        res = mSigningAccount;
    }
    else
    {
        res = make_shared<AccountFrame>();
        bool ok = AccountFrame::loadAccount(accountID, *res, app.getDatabase(), true);
        if (!ok)
        {
            res.reset();
        }
    }
    return res;
}

bool TransactionFrame::loadAccount(Application& app)
{
    mSigningAccount = loadAccount(app, getSourceID());
    return !!mSigningAccount;
}


OperationResultCode OperationFrame::getResultCode()
{
    return mResult.code();
}

// called when determining if we should accept this tx.
// called when determining if we should flood
// make sure sig is correct
// make sure maxFee is above the current fee
// make sure it is in the correct ledger bounds
// don't consider minBalance since you want to allow them to still send
// around credit etc
bool OperationFrame::checkValid(Application& app)
{
    if (!loadAccount(app))
    {
        mResult.code(opNO_ACCOUNT);
        return false;
    }

    if (!checkSignature())
    {
        mResult.code(opBAD_AUTH);
        return false;
    }

    mResult.code(opINNER);
    mResult.tr().type(
        mOperation.body.type());

    return doCheckValid(app);
}


bool TransactionFrame::checkValid(Application &app, bool applying)
{
    // pre-allocates the results for all operations
    mResult.result.code(txSUCCESS);
    mResult.result.results().resize((uint32_t)mEnvelope.tx.operations.size());

    mOperations.clear();

    // bind operations to the results
    for (size_t i = 0; i < mEnvelope.tx.operations.size(); i++)
    {
        mOperations.push_back(OperationFrame::makeHelper(mEnvelope.tx.operations[i],
            mResult.result.results()[i], *this));
    }

    // feeCharged is updated accordingly to represent the cost of the transaction
    // regardless of the failure modes.
    mResult.feeCharged = 0;

    if (mOperations.size() == 0)
    {
        mResult.result.code(txMALFORMED);
        return false;
    }

    if(mEnvelope.tx.maxLedger < app.getLedgerGateway().getLedgerNum())
    {
        mResult.result.code(txBAD_LEDGER);
        return false;
    }
    if(mEnvelope.tx.minLedger > app.getLedgerGateway().getLedgerNum())
    {
        mResult.result.code(txBAD_LEDGER);
        return false;
    }

    // fee we'd like to charge for this transaction
    int64_t fee = app.getLedgerGateway().getTxFee() * mOperations.size();

    if (mEnvelope.tx.maxFee < fee)
    {
        mResult.result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!loadAccount(app))
    {
        mResult.result.code(txNO_ACCOUNT);
        return false;
    }

    if (mSigningAccount->getAccount().balance < fee)
    {
        mResult.result.code(txINSUFFICIENT_BALANCE);
        return false;
    }

    if (applying)
    {
        // where seq != envelope.seq
        if (mSigningAccount->getSeq(mEnvelope.tx.seqSlot, app.getDatabase()) + 1 != mEnvelope.tx.seqNum)
        {
            mResult.result.code(txBAD_SEQ);
            return false;
        }
    }
    else
    {
        if (mSigningAccount->getSeq(mEnvelope.tx.seqSlot, app.getDatabase()) >= mEnvelope.tx.seqNum)
        {
            mResult.result.code(txBAD_SEQ);
            return false;
        }
    }

    if (!checkSignature(*mSigningAccount, mSigningAccount->getLowThreshold()))
    {
        mResult.result.code(txBAD_AUTH);
        return false;
    }

    // failures after this point will end up charging a fee if attempting to run "apply"
    mResult.feeCharged = fee;

    if (!applying)
    {
        for (auto &op : mOperations)
        {
            if (!op->checkValid(app))
            {
                return false;
            }
        }
    }

    return true;
}

void TransactionFrame::prepareResult(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    Database &db = ledgerMaster.getDatabase();
    int64_t fee = mResult.feeCharged;

    if (fee > 0)
    {
        if (mSigningAccount->getAccount().balance < fee)
        {
            // take all their balance to be safe
            fee = mSigningAccount->getAccount().balance;
        }
        mSigningAccount->setSeqSlot(mEnvelope.tx.seqSlot, mEnvelope.tx.seqNum);
        mSigningAccount->getAccount().balance -= fee;
        ledgerMaster.getCurrentLedgerHeader().feePool += fee;

        mSigningAccount->storeChange(delta, db);
    }
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

bool TransactionFrame::checkValid(Application& app)
{
    mSigningAccount.reset();
    return checkValid(app, false);
}

bool TransactionFrame::apply(LedgerDelta& delta, Application& app)
{
    mSigningAccount.reset();
    LedgerMaster &lm = app.getLedgerMaster();
    if (!checkValid(app, true))
    {
        prepareResult(delta, lm);
        return false;
    }
    // full fee charged at this point
    prepareResult(delta, lm);

    bool errorEncountered = false;

    {
        soci::transaction sqlTx(app.getDatabase().getSession());
        LedgerDelta thisTxDelta(delta);

        for (auto &op : mOperations)
        {
            bool txRes = op->apply(thisTxDelta, app);

            if (!txRes)
            {
                errorEncountered = true;
            }
        }

        if (!errorEncountered)
        {
            sqlTx.commit();
            thisTxDelta.commit();
        }
    }

    if (errorEncountered)
    {
        // changing "code" causes the xdr struct to be deleted/re-created
        // As we want to preserve the results, we save them inside a temp object
        // Also, note that because we're using move operators
        // mOperations are still valid (they have pointers to the individual results elements)
        xdr::xvector<OperationResult> t(std::move(mResult.result.results()));
        mResult.result.code(txFAILED);
        mResult.result.results() = std::move(t);
    }

    // return true as the transaction executed and collected a fee
    return true;
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

size_t
TransactionFrame::copyTransactionsToStream(Database& db,
                                           soci::session& sess,
                                           uint64_t ledgerSeq,
                                           uint64_t ledgerCount,
                                           XDROutputFileStream &out)
{
    auto timer = db.getSelectTimer("txhistory");
    std::string txBody, txResult, txMeta;
    uint64_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;
    TransactionHistoryEntry e;
    assert(begin <= end);
    soci::statement st =
        (sess.prepare <<
         "SELECT ledgerSeq, TxBody, TxResult FROM TxHistory "\
          "WHERE ledgerSeq >= :begin AND ledgerSeq < :end ",
         soci::into(e.ledgerSeq),
         soci::into(txBody), soci::into(txResult),
         soci::use(begin), soci::use(end));

    st.execute(true);
    while (st.got_data())
    {
        std::string body = base64::decode(txBody);
        std::string result = base64::decode(txResult);

        // FIXME: this +4 business is a bit embarassing.

        assert(body.size() >= 4);
        assert(result.size() >= 4);

        xdr::xdr_get g1(body.data() + 4, body.data() + body.size());
        xdr_argpack_archive(g1, e.envelope);

        xdr::xdr_get g2(result.data() + 4, result.data() + result.size());
        xdr_argpack_archive(g2, e.result);

        out.writeOne(e);
        ++n;
        st.fetch();
    }
    return n;
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
