// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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

TransactionFrame::pointer
TransactionFrame::makeTransactionFromWire(TransactionEnvelope const& msg)
{
    TransactionFrame::pointer res = make_shared<TransactionFrame>(msg);
    return res;
}

TransactionFrame::TransactionFrame(TransactionEnvelope const& envelope)
    : mEnvelope(envelope)
{
}

Hash const&
TransactionFrame::getFullHash() const
{
    if (isZero(mFullHash))
    {
        mFullHash = sha256(xdr::xdr_to_opaque(mEnvelope));
    }
    return (mFullHash);
}

Hash const&
TransactionFrame::getContentsHash() const
{
    if (isZero(mContentsHash))
    {
        mContentsHash = sha256(xdr::xdr_to_opaque(mEnvelope.tx));
    }
    return (mContentsHash);
}

void
TransactionFrame::clearCached()
{
    Hash zero;
    mContentsHash = zero;
    mFullHash = zero;
}

TransactionResultPair
TransactionFrame::getResultPair() const
{
    TransactionResultPair trp;
    trp.transactionHash = getContentsHash();
    trp.result = mResult;
    return trp;
}

TransactionEnvelope const&
TransactionFrame::getEnvelope() const
{
    return mEnvelope;
}

TransactionEnvelope&
TransactionFrame::getEnvelope()
{
    return mEnvelope;
}

int64_t
TransactionFrame::getFee(Application& app) const
{
    size_t count = mOperations.size();

    if (count == 0)
    {
        count = 1;
    }

    return app.getLedgerManager().getTxFee() * count;
}

void
TransactionFrame::addSignature(SecretKey const& secretKey)
{
    clearCached();
    DecoratedSignature sig;
    sig.signature = secretKey.sign(getContentsHash());
    memcpy(&sig.hint, secretKey.getPublicKey().data(), sizeof(sig.hint));
    mEnvelope.signatures.push_back(sig);
}

bool
TransactionFrame::checkSignature(AccountFrame& account, int32_t neededWeight)
{
    vector<Signer> keyWeights;
    if (account.getAccount().thresholds[0])
        keyWeights.push_back(
            Signer(account.getID(), account.getAccount().thresholds[0]));

    keyWeights.insert(keyWeights.end(), account.getAccount().signers.begin(),
                      account.getAccount().signers.end());

    Hash const& contentsHash = getContentsHash();

    // calculate the weight of the signatures
    int totalWeight = 0;

    for (size_t i = 0; i < getEnvelope().signatures.size(); i++)
    {
        auto const& sig = getEnvelope().signatures[i];

        for (auto it = keyWeights.begin(); it != keyWeights.end(); it++)
        {
            if ((std::memcmp(sig.hint.data(), (*it).pubKey.data(),
                             sizeof(sig.hint)) == 0) &&
                PublicKey::verifySig((*it).pubKey, sig.signature, contentsHash))
            {
                mUsedSignatures[i] = true;
                totalWeight += (*it).weight;
                if (totalWeight >= neededWeight)
                    return true;

                keyWeights.erase(it); // can't sign twice
                break;
            }
        }
    }

    return false;
}

AccountFrame::pointer
TransactionFrame::loadAccount(Application& app, AccountID const& accountID)
{
    AccountFrame::pointer res;

    if (mSigningAccount && mSigningAccount->getID() == accountID)
    {
        res = mSigningAccount;
    }
    else
    {
        res = make_shared<AccountFrame>();
        bool ok = AccountFrame::loadAccount(accountID, *res, app.getDatabase());
        if (!ok)
        {
            res.reset();
        }
    }
    return res;
}

bool
TransactionFrame::loadAccount(Application& app)
{
    mSigningAccount = loadAccount(app, getSourceID());
    return !!mSigningAccount;
}

bool
TransactionFrame::checkValid(Application& app, bool applying,
                             SequenceNumber current)
{
    // pre-allocates the results for all operations
    getResult().result.code(txSUCCESS);
    getResult().result.results().resize(
        (uint32_t)mEnvelope.tx.operations.size());

    mOperations.clear();

    // bind operations to the results
    for (size_t i = 0; i < mEnvelope.tx.operations.size(); i++)
    {
        mOperations.push_back(
            OperationFrame::makeHelper(mEnvelope.tx.operations[i],
                                       getResult().result.results()[i], *this));
    }

    // feeCharged is updated accordingly to represent the cost of the
    // transaction
    // regardless of the failure modes.
    getResult().feeCharged = 0;

    if (mOperations.size() == 0)
    {
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

    if (mEnvelope.tx.maxLedger < app.getLedgerManager().getLedgerNum())
    {
        getResult().result.code(txBAD_LEDGER);
        return false;
    }
    if (mEnvelope.tx.minLedger > app.getLedgerManager().getLedgerNum())
    {
        getResult().result.code(txBAD_LEDGER);
        return false;
    }

    // fee we'd like to charge for this transaction
    int64_t fee = getFee(app);

    if (mEnvelope.tx.maxFee < fee)
    {
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!loadAccount(app))
    {
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    if (current == 0)
    {
        current = mSigningAccount->getSeqNum();
    }

    if (current + 1 != mEnvelope.tx.seqNum)
    {
        getResult().result.code(txBAD_SEQ);
        return false;
    }

    if (!checkSignature(*mSigningAccount, mSigningAccount->getLowThreshold()))
    {
        getResult().result.code(txBAD_AUTH);
        return false;
    }

    // failures after this point will end up charging a fee if attempting to run
    // "apply"
    getResult().feeCharged = fee;

    // don't let the account go below the reserve
    if (mSigningAccount->getAccount().balance - fee <
        mSigningAccount->getMinimumBalance(app.getLedgerManager()))
    {
        getResult().result.code(txINSUFFICIENT_BALANCE);
        return false;
    }

    if (!applying)
    {
        for (auto& op : mOperations)
        {
            if (!op->checkValid(app))
            {
                // it's OK to just fast fail here and not try to call
                // checkValid on all operations as the resulting object
                // is only used by applications
                markResultFailed();
                return false;
            }
        }
        return checkAllSignaturesUsed();
    }

    return true;
}

void
TransactionFrame::prepareResult(LedgerDelta& delta,
                                LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();
    int64_t fee = getResult().feeCharged;

    if (fee > 0)
    {
        int64_t avail = mSigningAccount->getBalanceAboveReserve(ledgerManager);
        if (avail < fee)
        {
            // take all their balance to be safe
            fee = avail;
        }
        mSigningAccount->setSeqNum(mEnvelope.tx.seqNum);
        mSigningAccount->getAccount().balance -= fee;
        delta.getHeader().feePool += fee;

        mSigningAccount->storeChange(delta, db);
    }
}

void
TransactionFrame::setSourceAccountPtr(AccountFrame::pointer signingAccount)
{
    if (!signingAccount)
    {
        if (mEnvelope.tx.sourceAccount != signingAccount->getID())
        {
            throw std::invalid_argument("wrong account");
        }
    }
    mSigningAccount = signingAccount;
}

void
TransactionFrame::resetState()
{
    mSigningAccount.reset();
    mUsedSignatures = std::vector<bool>(mEnvelope.signatures.size());
}

bool
TransactionFrame::checkAllSignaturesUsed()
{
    for (auto sigb : mUsedSignatures)
    {
        if (!sigb)
        {
            getResult().result.code(txBAD_AUTH_EXTRA);
            return false;
        }
    }
    return true;
}

bool
TransactionFrame::checkValid(Application& app, SequenceNumber current)
{
    resetState();
    return checkValid(app, false, current);
}

void
TransactionFrame::markResultFailed()
{
    // changing "code" causes the xdr struct to be deleted/re-created
    // As we want to preserve the results, we save them inside a temp object
    // Also, note that because we're using move operators
    // mOperations are still valid (they have pointers to the individual
    // results elements)
    xdr::xvector<OperationResult> t(std::move(getResult().result.results()));
    getResult().result.code(txFAILED);
    getResult().result.results() = std::move(t);
}

bool
TransactionFrame::apply(LedgerDelta& delta, Application& app)
{
    resetState();
    LedgerManager& lm = app.getLedgerManager();
    if (!checkValid(app, true, 0))
    {
        prepareResult(delta, lm);
        return false;
    }
    // full fee charged at this point
    prepareResult(delta, lm);

    bool errorEncountered = false;

    {
        // shield outer scope of any side effects by using
        // a sql transaction for ledger state and LedgerDelta
        soci::transaction sqlTx(app.getDatabase().getSession());
        LedgerDelta thisTxDelta(delta);

        for (auto& op : mOperations)
        {
            bool txRes = op->apply(thisTxDelta, app);

            if (!txRes)
            {
                errorEncountered = true;
            }
        }

        if (!errorEncountered)
        {
            if (!checkAllSignaturesUsed())
            {
                // this should never happen: malformed transaction should not be
                // accepted by nodes
                return false;
            }

            sqlTx.commit();
            thisTxDelta.commit();
        }
    }

    if (errorEncountered)
    {
        markResultFailed();
    }

    // return true as the transaction executed and collected a fee
    return true;
}

StellarMessage
TransactionFrame::toStellarMessage() const
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction() = mEnvelope;
    return msg;
}

void
TransactionFrame::storeTransaction(LedgerManager& ledgerManager,
                                   LedgerDelta const& delta, int txindex,
                                   SHA256& resultHasher) const
{
    auto txBytes(xdr::xdr_to_opaque(mEnvelope));
    auto txResultBytes(xdr::xdr_to_opaque(getResultPair()));

    resultHasher.add(txResultBytes);

    std::string txBody = base64::encode(
        reinterpret_cast<const unsigned char*>(txBytes.data()), txBytes.size());

    std::string txResult = base64::encode(
        reinterpret_cast<const unsigned char*>(txResultBytes.data()),
        txResultBytes.size());

    xdr::opaque_vec<> txMeta(delta.getTransactionMeta());

    std::string meta = base64::encode(
        reinterpret_cast<const unsigned char*>(txMeta.data()), txMeta.size());

    string txIDString(binToHex(getContentsHash()));

    auto timer = ledgerManager.getDatabase().getInsertTimer("txhistory");
    soci::statement st =
        (ledgerManager.getDatabase().getSession().prepare
             << "INSERT INTO TxHistory (txID, ledgerSeq, txindex, TxBody, "
                "TxResult, TxMeta) VALUES "
                "(:id,:seq,:txindex,:txb,:txres,:meta)",
         soci::use(txIDString),
         soci::use(ledgerManager.getCurrentLedgerHeader().ledgerSeq),
         soci::use(txindex), soci::use(txBody), soci::use(txResult),
         soci::use(meta));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

static void
saveTransactionHelper(Database& db, soci::session& sess, uint32 ledgerSeq,
                      TxSetFrame& txSet, TransactionHistoryResultEntry& results,
                      XDROutputFileStream& txOut,
                      XDROutputFileStream& txResultOut)
{
    // prepare the txset for saving
    LedgerHeaderFrame::pointer lh =
        LedgerHeaderFrame::loadBySequence(ledgerSeq, db, sess);
    if (!lh)
    {
        throw std::runtime_error("Could not find ledger");
    }
    txSet.previousLedgerHash() = lh->mHeader.previousLedgerHash;
    txSet.sortForHash();
    TransactionHistoryEntry hist;
    hist.ledgerSeq = ledgerSeq;
    txSet.toXDR(hist.txSet);
    txOut.writeOne(hist);

    txResultOut.writeOne(results);
}

size_t
TransactionFrame::copyTransactionsToStream(Database& db, soci::session& sess,
                                           uint32_t ledgerSeq,
                                           uint32_t ledgerCount,
                                           XDROutputFileStream& txOut,
                                           XDROutputFileStream& txResultOut)
{
    auto timer = db.getSelectTimer("txhistory");
    std::string txBody, txResult, txMeta;
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    TransactionEnvelope tx;
    uint32_t curLedgerSeq;

    assert(begin <= end);
    soci::statement st =
        (sess.prepare << "SELECT ledgerSeq, TxBody, TxResult FROM TxHistory "
                         "WHERE ledgerSeq >= :begin AND ledgerSeq < :end ORDER "
                         "BY ledgerSeq ASC, txindex ASC",
         soci::into(curLedgerSeq), soci::into(txBody), soci::into(txResult),
         soci::use(begin), soci::use(end));

    Hash h;
    TxSetFrame txSet(h); // we're setting the hash later
    TransactionHistoryResultEntry results;

    st.execute(true);

    uint32_t lastLedgerSeq = curLedgerSeq;
    results.ledgerSeq = curLedgerSeq;

    while (st.got_data())
    {
        if (curLedgerSeq != lastLedgerSeq)
        {
            saveTransactionHelper(db, sess, lastLedgerSeq, txSet, results,
                                  txOut, txResultOut);
            // reset state
            txSet.mTransactions.clear();
            results.ledgerSeq = ledgerSeq;
            results.txResultSet.results.clear();
            lastLedgerSeq = curLedgerSeq;
        }

        std::string body = base64::decode(txBody);
        std::string result = base64::decode(txResult);

        xdr::xdr_get g1(body.data(), body.data() + body.size());
        xdr_argpack_archive(g1, tx);

        TransactionFramePtr txFrame = make_shared<TransactionFrame>(tx);
        txSet.add(txFrame);

        xdr::xdr_get g2(result.data(), result.data() + result.size());
        results.txResultSet.results.emplace_back();

        TransactionResultPair& p = results.txResultSet.results.back();
        xdr_argpack_archive(g2, p);

        if (p.transactionHash != txFrame->getContentsHash())
        {
            throw std::runtime_error("transaction mismatch");
        }

        ++n;
        st.fetch();
    }
    if (n != 0)
    {
        saveTransactionHelper(db, sess, lastLedgerSeq, txSet, results, txOut,
                              txResultOut);
    }
    return n;
}

void
TransactionFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS TxHistory";

    db.getSession() << "CREATE TABLE TxHistory ("
                       "txID          CHARACTER(64) NOT NULL,"
                       "ledgerSeq     INT NOT NULL CHECK (ledgerSeq >= 0),"
                       "txindex         INT NOT NULL,"
                       "TxBody        TEXT NOT NULL,"
                       "TxResult      TEXT NOT NULL,"
                       "TxMeta        TEXT NOT NULL,"
                       "PRIMARY KEY (txID, ledgerSeq),"
                       "UNIQUE      (ledgerSeq, txindex)"
                       ")";
}
}
