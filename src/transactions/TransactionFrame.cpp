// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "TransactionFrame.h"
#include "OperationFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "ledger/LedgerDelta.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "crypto/Hex.h"
#include <cereal/external/base64.hpp>

namespace stellar
{

using namespace std;

TransactionFrame::pointer TransactionFrame::makeTransactionFromWire(TransactionEnvelope const& msg)
{
    TransactionFrame::pointer res = make_shared<TransactionFrame>(msg);
    return res;
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

void TransactionFrame::addSignature(const SecretKey& secretKey)
{
    uint512 sig = secretKey.sign(getContentsHash());
    mEnvelope.signatures.push_back(sig);
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
        if (mSigningAccount->getSeqNum(app.getDatabase()) + 1 != mEnvelope.tx.seqNum)
        {
            mResult.result.code(txBAD_SEQ);
            return false;
        }
    }
    else
    {
        if (mSigningAccount->getSeqNum(app.getDatabase()) >= mEnvelope.tx.seqNum)
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
        mSigningAccount->setSeqNum(mEnvelope.tx.seqNum);
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
    auto txBytes(xdr::xdr_to_opaque(mEnvelope));
    auto txResultBytes(xdr::xdr_to_opaque(mResult));

    std::string txBody = base64::encode(
        reinterpret_cast<const unsigned char *>(txBytes.data()),
        txBytes.size());

    std::string txResult = base64::encode(
        reinterpret_cast<const unsigned char *>(txResultBytes.data()),
        txResultBytes.size());

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

        xdr::xdr_get g1(body.data(), body.data() + body.size());
        xdr_argpack_archive(g1, e.envelope);

        xdr::xdr_get g2(result.data(), result.data() + result.size());
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
