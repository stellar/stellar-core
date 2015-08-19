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
#include "herder/TxSetFrame.h"
#include "crypto/Hex.h"
#include "util/basen.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

TransactionFramePtr
TransactionFrame::makeTransactionFromWire(TransactionEnvelope const& msg)
{
    TransactionFramePtr res = make_shared<TransactionFrame>(msg);
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
        mContentsHash =
            sha256(xdr::xdr_to_opaque(ENVELOPE_TYPE_TX, mEnvelope.tx));
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

float
TransactionFrame::getFeeRatio(Application& app) const
{
    return ((float)getFee() / (float)getMinFee(app));
}

int64_t
TransactionFrame::getFee() const
{
    return mEnvelope.tx.fee;
}

int64_t
TransactionFrame::getMinFee(Application& app) const
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
    sig.hint = PubKeyUtils::getHint(secretKey.getPublicKey());
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
            if (PubKeyUtils::hasHint((*it).pubKey, sig.hint) &&
                PubKeyUtils::verifySig((*it).pubKey, sig.signature,
                                       contentsHash))
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
        res = AccountFrame::loadAccount(accountID, app.getDatabase());
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
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "missing-operation"},
                      "transaction")
            .Mark();
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

    if (mEnvelope.tx.timeBounds)
    {
        uint64 closeTime =
            app.getLedgerManager().getCurrentLedgerHeader().scpValue.closeTime;
        if (mEnvelope.tx.timeBounds->minTime > closeTime)
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "too-early"},
                          "transaction")
                .Mark();
            getResult().result.code(txTOO_EARLY);
            return false;
        }
        if (mEnvelope.tx.timeBounds->maxTime &&
            (mEnvelope.tx.timeBounds->maxTime < closeTime))
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "too-late"}, "transaction")
                .Mark();
            getResult().result.code(txTOO_LATE);
            return false;
        }
    }

    if (mEnvelope.tx.fee < getMinFee(app))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "insufficient-fee"},
                      "transaction")
            .Mark();
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!loadAccount(app))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "no-account"}, "transaction")
            .Mark();
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    if (current == 0)
    {
        current = mSigningAccount->getSeqNum();
    }

    if (current + 1 != mEnvelope.tx.seqNum)
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "bad-seq"}, "transaction")
            .Mark();
        getResult().result.code(txBAD_SEQ);
        return false;
    }

    if (!checkSignature(*mSigningAccount, mSigningAccount->getLowThreshold()))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "bad-auth"}, "transaction")
            .Mark();
        getResult().result.code(txBAD_AUTH);
        return false;
    }

    // failures after this point will end up charging a fee if attempting to run
    // "apply"
    getResult().feeCharged = mEnvelope.tx.fee;

    // don't let the account go below the reserve
    if (mSigningAccount->getAccount().balance - mEnvelope.tx.fee <
        mSigningAccount->getMinimumBalance(app.getLedgerManager()))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "insufficient-balance"},
                      "transaction")
            .Mark();
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
                app.getMetrics()
                    .NewMeter({"transaction", "invalid", "invalid-op"},
                              "transaction")
                    .Mark();
                markResultFailed();
                return false;
            }
        }
        auto b = checkAllSignaturesUsed();
        if (!b)
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "bad-auth-extra"},
                          "transaction")
                .Mark();
        }
        return b;
    }

    return true;
}

void
TransactionFrame::prepareResult(LedgerDelta& delta,
                                LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();
    int64_t& fee = getResult().feeCharged;

    if (fee > 0)
    {
        int64_t avail = mSigningAccount->getAccount().balance;
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
        if (!(mEnvelope.tx.sourceAccount == signingAccount->getID()))
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
    TransactionMeta tm;
    return apply(delta, tm, app);
}

bool
TransactionFrame::apply(LedgerDelta& delta, TransactionMeta& tm,
                        Application& app)
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

    auto& meta = tm.v0();

    meta.changes = delta.getChanges();

    bool errorEncountered = false;

    {
        // shield outer scope of any side effects by using
        // a sql transaction for ledger state and LedgerDelta
        soci::transaction sqlTx(app.getDatabase().getSession());
        LedgerDelta thisTxDelta(delta);

        auto& opTimer = app.getMetrics().NewTimer(
            {"transaction", "op", "apply"});

        for (auto& op : mOperations)
        {
            auto time = opTimer.TimeScope();
            LedgerDelta opDelta(thisTxDelta);
            bool txRes = op->apply(opDelta, app);

            if (!txRes)
            {
                errorEncountered = true;
            }
            meta.operations.emplace_back(opDelta.getChanges());
            opDelta.commit();
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
        meta.operations.clear();
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
                                   LedgerDelta const& delta,
                                   TransactionMeta& tm, int txindex,
                                   TransactionResultSet& resultSet) const
{
    auto txBytes(xdr::xdr_to_opaque(mEnvelope));

    resultSet.results.emplace_back(getResultPair());
    auto txResultBytes(xdr::xdr_to_opaque(resultSet.results.back()));

    std::string txBody;
    txBody = bn::encode_b64(txBytes);

    std::string txResult;
    txResult = bn::encode_b64(txResultBytes);

    xdr::opaque_vec<> txMeta(xdr::xdr_to_opaque(tm));

    std::string meta;
    meta = bn::encode_b64(txMeta);

    string txIDString(binToHex(getContentsHash()));

    auto& db = ledgerManager.getDatabase();
    auto prep = db.getPreparedStatement(
        "INSERT INTO txhistory "
        "( txid, ledgerseq, txindex,  txbody, txresult, txmeta) VALUES "
        "(:id,  :seq,      :txindex, :txb,   :txres,   :meta)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerManager.getCurrentLedgerHeader().ledgerSeq));
    st.exchange(soci::use(txindex));
    st.exchange(soci::use(txBody));
    st.exchange(soci::use(txResult));
    st.exchange(soci::use(meta));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("txhistory");
        st.execute(true);
    }

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
        (sess.prepare << "SELECT ledgerseq, txbody, txresult FROM txhistory "
                         "WHERE ledgerseq >= :begin AND ledgerseq < :end ORDER "
                         "BY ledgerseq ASC, txindex ASC",
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
            results.ledgerSeq = curLedgerSeq;
            results.txResultSet.results.clear();
            lastLedgerSeq = curLedgerSeq;
        }

        std::vector<uint8_t> body;
        bn::decode_b64(txBody, body);

        std::vector<uint8_t> result;
        bn::decode_b64(txResult, result);

        xdr::xdr_get g1(&body.front(), &body.back() + 1);
        xdr_argpack_archive(g1, tx);

        TransactionFramePtr txFrame = make_shared<TransactionFrame>(tx);
        txSet.add(txFrame);

        xdr::xdr_get g2(&result.front(), &result.back() + 1);
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
    db.getSession() << "DROP TABLE IF EXISTS txhistory";

    db.getSession() << "CREATE TABLE txhistory ("
                       "txid          CHARACTER(64) NOT NULL,"
                       "ledgerseq     INT NOT NULL CHECK (ledgerseq >= 0),"
                       "txindex         INT NOT NULL,"
                       "txbody        TEXT NOT NULL,"
                       "txresult      TEXT NOT NULL,"
                       "txmeta        TEXT NOT NULL,"
                       "PRIMARY KEY (txid, ledgerseq),"
                       "UNIQUE      (ledgerseq, txindex)"
                       ")";
}
}
