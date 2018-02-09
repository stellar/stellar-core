// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TransactionFrame.h"
#include "OperationFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "herder/TxSetFrame.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "util/Algoritm.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "util/basen.h"
#include "xdrpp/marshal.h"
#include <string>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <numeric>

namespace stellar
{

using namespace std;
using xdr::operator==;

TransactionFramePtr
TransactionFrame::makeTransactionFromWire(Hash const& networkID,
                                          TransactionEnvelope const& msg)
{
    TransactionFramePtr res = make_shared<TransactionFrame>(networkID, msg);
    return res;
}

TransactionFrame::TransactionFrame(Hash const& networkID,
                                   TransactionEnvelope const& envelope)
    : mEnvelope(envelope), mNetworkID(networkID)
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
        mContentsHash = sha256(
            xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_TX, mEnvelope.tx));
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

double
TransactionFrame::getFeeRatio(LedgerManager const& lm) const
{
    return ((double)getFee() / (double)getMinFee(lm));
}

uint32_t
TransactionFrame::getFee() const
{
    return mEnvelope.tx.fee;
}

int64_t
TransactionFrame::getMinFee(LedgerManager const& lm) const
{
    size_t count = mOperations.size();

    if (count == 0)
    {
        count = 1;
    }

    return lm.getTxFee() * count;
}

void
TransactionFrame::addSignature(SecretKey const& secretKey)
{
    clearCached();
    auto sig = SignatureUtils::sign(secretKey, getContentsHash());
    addSignature(sig);
}

void
TransactionFrame::addSignature(DecoratedSignature const& signature)
{
    mEnvelope.signatures.push_back(signature);
}

bool
TransactionFrame::checkSignature(SignatureChecker& signatureChecker,
                                 AccountFrame& account, int32_t neededWeight)
{
    std::vector<Signer> signers;
    if (account.getAccount().thresholds[0])
        signers.push_back(
            Signer(KeyUtils::convertKey<SignerKey>(account.getID()),
                   account.getAccount().thresholds[0]));
    signers.insert(signers.end(), account.getAccount().signers.begin(),
                   account.getAccount().signers.end());

    return signatureChecker.checkSignature(account.getID(), signers,
                                           neededWeight);
}

AccountFrame::pointer
TransactionFrame::loadAccount(int ledgerProtocolVersion, LedgerDelta* delta,
                              Database& db, AccountID const& accountID)
{
    AccountFrame::pointer res;

    if (ledgerProtocolVersion < 8 && mSigningAccount &&
        mSigningAccount->getID() == accountID)
    {
        // this is buggy caching that existed in old versions of the protocol
        res = mSigningAccount;
    }
    else if (delta)
    {
        res = AccountFrame::loadAccount(*delta, accountID, db);
    }
    else
    {
        res = AccountFrame::loadAccount(accountID, db);
    }
    return res;
}

bool
TransactionFrame::loadAccount(int ledgerProtocolVersion, LedgerDelta* delta,
                              Database& db)
{
    mSigningAccount =
        loadAccount(ledgerProtocolVersion, delta, db, getSourceID());
    return !!mSigningAccount;
}

void
TransactionFrame::resetResults()
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
    // transaction regardless of the failure modes.
    getResult().feeCharged = getFee();
}

bool
TransactionFrame::commonValidPreSeqNum(Application& app, LedgerDelta* delta)
{
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    if (mOperations.size() == 0)
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "missing-operation"},
                      "transaction")
            .Mark();
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

    auto& lm = app.getLedgerManager();
    if (mEnvelope.tx.timeBounds)
    {
        uint64 closeTime = lm.getCurrentLedgerHeader().scpValue.closeTime;
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

    if (mEnvelope.tx.fee < getMinFee(lm))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "insufficient-fee"},
                      "transaction")
            .Mark();
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!loadAccount(app.getLedgerManager().getCurrentLedgerVersion(), delta,
                     app.getDatabase()))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "no-account"}, "transaction")
            .Mark();
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    return true;
}

void
TransactionFrame::processSeqNum(LedgerManager& lm, LedgerDelta& delta)
{
    if (lm.getCurrentLedgerVersion() >= 10)
    {
        if (mSigningAccount->getSeqNum() > mEnvelope.tx.seqNum)
        {
            throw std::runtime_error("unexpected sequence number");
        }
        mSigningAccount->setSeqNum(mEnvelope.tx.seqNum);
        mSigningAccount->storeChange(delta, lm.getDatabase());
    }
}

TransactionFrame::ValidationType
TransactionFrame::commonValid(SignatureChecker& signatureChecker,
                              Application& app, LedgerDelta* delta,
                              SequenceNumber current)
{
    ValidationType res = ValidationType::kInvalid;

    bool applying = (delta != nullptr);

    if (!commonValidPreSeqNum(app, delta))
    {
        return res;
    }

    auto& lm = app.getLedgerManager();

    // in older versions, the account's sequence number is updated when taking
    // fees
    if (lm.getCurrentLedgerVersion() >= 10 || !applying)
    {
        if (current == 0)
        {
            current = mSigningAccount->getSeqNum();
        }
        if (current == INT64_MAX || current + 1 != mEnvelope.tx.seqNum)
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "bad-seq"}, "transaction")
                .Mark();
            getResult().result.code(txBAD_SEQ);
            return res;
        }
    }

    res = ValidationType::kInvalidUpdateSeqNum;

    if (!checkSignature(signatureChecker, *mSigningAccount,
                        mSigningAccount->getLowThreshold()))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "bad-auth"}, "transaction")
            .Mark();
        getResult().result.code(txBAD_AUTH);
        return res;
    }

    // if we are in applying mode fee was already deduced from signing account
    // balance, if not, we need to check if after that deduction this account
    // will still have minimum balance
    auto balanceAfter =
        (applying && (lm.getCurrentLedgerVersion() > 8))
            ? mSigningAccount->getAccount().balance
            : mSigningAccount->getAccount().balance - mEnvelope.tx.fee;

    // don't let the account go below the reserve
    if (balanceAfter < mSigningAccount->getMinimumBalance(lm))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "insufficient-balance"},
                      "transaction")
            .Mark();
        getResult().result.code(txINSUFFICIENT_BALANCE);
        return res;
    }

    return ValidationType::kFullyValid;
}

void
TransactionFrame::processFeeSeqNum(LedgerDelta& delta,
                                   LedgerManager& ledgerManager)
{
    resetSigningAccount();
    resetResults();

    if (!loadAccount(ledgerManager.getCurrentLedgerVersion(), &delta,
                     ledgerManager.getDatabase()))
    {
        throw std::runtime_error("Unexpected database state");
    }

    Database& db = ledgerManager.getDatabase();
    int64_t& fee = getResult().feeCharged;

    if (fee > 0)
    {
        fee = std::min(mSigningAccount->getAccount().balance, fee);
        mSigningAccount->addBalance(-fee);
        delta.getHeader().feePool += fee;
    }
    // in v10 we update sequence numbers during apply
    if (ledgerManager.getCurrentLedgerVersion() <= 9)
    {
        if (mSigningAccount->getSeqNum() + 1 != mEnvelope.tx.seqNum)
        {
            // this should not happen as the transaction set is sanitized for
            // sequence numbers
            throw std::runtime_error("Unexpected account state");
        }
        mSigningAccount->setSeqNum(mEnvelope.tx.seqNum);
    }
    mSigningAccount->storeChange(delta, db);
}

void
TransactionFrame::resetSigningAccount()
{
    mSigningAccount.reset();
}

void
TransactionFrame::removeUsedOneTimeSignerKeys(
    SignatureChecker& signatureChecker, LedgerDelta& delta,
    LedgerManager& ledgerManager)
{
    for (auto const& usedAccount : signatureChecker.usedOneTimeSignerKeys())
    {
        removeUsedOneTimeSignerKeys(usedAccount.first, usedAccount.second,
                                    delta, ledgerManager);
    }
}

void
TransactionFrame::removeUsedOneTimeSignerKeys(
    const AccountID& accountId, const std::set<SignerKey>& keys,
    LedgerDelta& delta, LedgerManager& ledgerManager) const
{
    auto account =
        AccountFrame::loadAccount(accountId, ledgerManager.getDatabase());
    if (!account)
    {
        return; // probably account was removed due to merge operation
    }

    auto changed = std::accumulate(
        std::begin(keys), std::end(keys), false,
        [&](bool r, const SignerKey& signerKey) {
            return r || removeAccountSigner(account, signerKey, ledgerManager);
        });

    if (changed)
    {
        account->setUpdateSigners();
        account->storeChange(delta, ledgerManager.getDatabase());
    }
}

bool
TransactionFrame::removeAccountSigner(const AccountFrame::pointer& account,
                                      const SignerKey& signerKey,
                                      LedgerManager& ledgerManager) const
{
    auto& signers = account->getAccount().signers;
    auto it = std::find_if(
        std::begin(signers), std::end(signers),
        [&signerKey](Signer const& signer) { return signer.key == signerKey; });
    if (it != std::end(signers))
    {
        auto removed = account->addNumEntries(-1, ledgerManager);
        assert(removed);
        signers.erase(it);
        return true;
    }

    return false;
}

bool
TransactionFrame::checkValid(Application& app, SequenceNumber current)
{
    resetSigningAccount();
    resetResults();
    SignatureChecker signatureChecker{
        app.getLedgerManager().getCurrentLedgerVersion(), getContentsHash(),
        mEnvelope.signatures};
    bool res = commonValid(signatureChecker, app, nullptr, current) ==
               ValidationType::kFullyValid;
    if (res)
    {
        for (auto& op : mOperations)
        {
            if (!op->checkValid(signatureChecker, app))
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

        if (!signatureChecker.checkAllSignaturesUsed())
        {
            res = false;
            getResult().result.code(txBAD_AUTH_EXTRA);
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "bad-auth-extra"},
                          "transaction")
                .Mark();
        }
    }
    return res;
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

    // sanity check in case some implementations decide
    // to not implement std::move properly
    auto const& allResults = getResult().result.results();
    assert(allResults.size() == mOperations.size());
    for (size_t i = 0; i < mOperations.size(); i++)
    {
        assert(&mOperations[i]->getResult() == &allResults[i]);
    }
}

bool
TransactionFrame::apply(LedgerDelta& delta, Application& app)
{
    TransactionMeta tm(1);
    return apply(delta, tm.v1(), app);
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  LedgerDelta& delta, TransactionMetaV1& meta,
                                  Application& app)
{
    bool errorEncountered = false;

    {
        // shield outer scope of any side effects by using
        // a sql transaction for ledger state and LedgerDelta
        soci::transaction sqlTx(app.getDatabase().getSession());
        LedgerDelta thisTxOpsDelta(delta);

        auto& opTimer =
            app.getMetrics().NewTimer({"transaction", "op", "apply"});

        for (auto& op : mOperations)
        {
            auto time = opTimer.TimeScope();
            LedgerDelta opDelta(thisTxOpsDelta);
            bool txRes = op->apply(signatureChecker, opDelta, app);

            if (!txRes)
            {
                errorEncountered = true;
            }
            if (!errorEncountered)
            {
                app.getInvariantManager().checkOnOperationApply(
                    op->getOperation(), op->getResult(), opDelta);
            }
            meta.operations.emplace_back(opDelta.getChanges());
            opDelta.commit();
        }

        if (!errorEncountered)
        {
            if (!signatureChecker.checkAllSignaturesUsed())
            {
                getResult().result.code(txBAD_AUTH_EXTRA);
                // this should never happen: malformed transaction should not be
                // accepted by nodes
                return false;
            }

            // if an error occurred, it is responsibility of account's owner to
            // remove that signer
            removeUsedOneTimeSignerKeys(signatureChecker, thisTxOpsDelta,
                                        app.getLedgerManager());
            sqlTx.commit();
            thisTxOpsDelta.commit();
        }
    }

    if (errorEncountered)
    {
        meta.operations.clear();
        markResultFailed();
    }

    return !errorEncountered;
}

bool
TransactionFrame::apply(LedgerDelta& delta, TransactionMetaV1& meta,
                        Application& app)
{
    resetSigningAccount();
    SignatureChecker signatureChecker{
        app.getLedgerManager().getCurrentLedgerVersion(), getContentsHash(),
        mEnvelope.signatures};

    bool valid;
    {
        LedgerDelta txDelta(delta);
        // when applying, a failure during tx validation means that
        // we'll skip trying to apply operations but we'll still
        // process the sequence number if needed
        auto cv = commonValid(signatureChecker, app, &txDelta, 0);
        if (cv >= ValidationType::kInvalidUpdateSeqNum)
        {
            processSeqNum(app.getLedgerManager(), txDelta);
        }
        meta.txChanges = txDelta.getChanges();
        txDelta.commit();
        valid = (cv == ValidationType::kFullyValid);
    }
    return valid && applyOperations(signatureChecker, delta, meta, app);
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

void
TransactionFrame::storeTransactionFee(LedgerManager& ledgerManager,
                                      LedgerEntryChanges const& changes,
                                      int txindex) const
{
    xdr::opaque_vec<> txChanges(xdr::xdr_to_opaque(changes));

    std::string txChanges64;
    txChanges64 = bn::encode_b64(txChanges);

    string txIDString(binToHex(getContentsHash()));

    auto& db = ledgerManager.getDatabase();
    auto prep = db.getPreparedStatement(
        "INSERT INTO txfeehistory "
        "( txid, ledgerseq, txindex,  txchanges) VALUES "
        "(:id,  :seq,      :txindex, :txchanges)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerManager.getCurrentLedgerHeader().ledgerSeq));
    st.exchange(soci::use(txindex));
    st.exchange(soci::use(txChanges64));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("txfeehistory");
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

TransactionResultSet
TransactionFrame::getTransactionHistoryResults(Database& db, uint32 ledgerSeq)
{
    TransactionResultSet res;
    std::string txresult64;
    auto prep =
        db.getPreparedStatement("SELECT txresult FROM txhistory "
                                "WHERE ledgerseq = :lseq ORDER BY txindex ASC");
    auto& st = prep.statement();

    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::into(txresult64));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        std::vector<uint8_t> result;
        bn::decode_b64(txresult64, result);

        res.results.emplace_back();
        TransactionResultPair& p = res.results.back();

        xdr::xdr_get g(&result.front(), &result.back() + 1);
        xdr_argpack_archive(g, p);

        st.fetch();
    }
    return res;
}

std::vector<LedgerEntryChanges>
TransactionFrame::getTransactionFeeMeta(Database& db, uint32 ledgerSeq)
{
    std::vector<LedgerEntryChanges> res;
    std::string changes64;
    auto prep =
        db.getPreparedStatement("SELECT txchanges FROM txfeehistory "
                                "WHERE ledgerseq = :lseq ORDER BY txindex ASC");
    auto& st = prep.statement();

    st.exchange(soci::into(changes64));
    st.exchange(soci::use(ledgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        std::vector<uint8_t> changesRaw;
        bn::decode_b64(changes64, changesRaw);

        xdr::xdr_get g1(&changesRaw.front(), &changesRaw.back() + 1);
        res.emplace_back();
        xdr_argpack_archive(g1, res.back());

        st.fetch();
    }
    return res;
}

size_t
TransactionFrame::copyTransactionsToStream(Hash const& networkID, Database& db,
                                           soci::session& sess,
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

        TransactionFramePtr txFrame =
            make_shared<TransactionFrame>(networkID, tx);
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

    db.getSession() << "DROP TABLE IF EXISTS txfeehistory";

    db.getSession() << "CREATE TABLE txhistory ("
                       "txid        CHARACTER(64) NOT NULL,"
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "txindex     INT NOT NULL,"
                       "txbody      TEXT NOT NULL,"
                       "txresult    TEXT NOT NULL,"
                       "txmeta      TEXT NOT NULL,"
                       "PRIMARY KEY (ledgerseq, txindex)"
                       ")";
    db.getSession() << "CREATE INDEX histbyseq ON txhistory (ledgerseq);";

    db.getSession() << "CREATE TABLE txfeehistory ("
                       "txid        CHARACTER(64) NOT NULL,"
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "txindex     INT NOT NULL,"
                       "txchanges   TEXT NOT NULL,"
                       "PRIMARY KEY (ledgerseq, txindex)"
                       ")";
    db.getSession() << "CREATE INDEX histfeebyseq ON txfeehistory (ledgerseq);";
}

void
TransactionFrame::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                   uint32_t count)
{
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "txhistory", "ledgerseq");
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "txfeehistory", "ledgerseq");
}
}
