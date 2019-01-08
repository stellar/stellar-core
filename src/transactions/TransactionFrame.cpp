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
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Algoritm.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/XDRStream.h"
#include "xdrpp/marshal.h"
#include <string>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <numeric>

namespace stellar
{

using namespace std;

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

uint32_t
TransactionFrame::getFee() const
{
    return mEnvelope.tx.fee;
}

int64_t
TransactionFrame::getMinFee(LedgerTxnHeader const& header) const
{
    return ((int64_t)header.current().baseFee) *
           std::max<int64_t>(1, mOperations.size());
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
                                 LedgerTxnEntry const& account,
                                 int32_t neededWeight)
{
    auto& acc = account.current().data.account();
    std::vector<Signer> signers;
    if (acc.thresholds[0])
    {
        auto signerKey = KeyUtils::convertKey<SignerKey>(acc.accountID);
        signers.push_back(Signer(signerKey, acc.thresholds[0]));
    }
    signers.insert(signers.end(), acc.signers.begin(), acc.signers.end());

    return signatureChecker.checkSignature(acc.accountID, signers,
                                           neededWeight);
}

bool
TransactionFrame::checkSignatureNoAccount(SignatureChecker& signatureChecker,
                                          AccountID const& accountID)
{
    std::vector<Signer> signers;
    auto signerKey = KeyUtils::convertKey<SignerKey>(accountID);
    signers.push_back(Signer(signerKey, 1));
    return signatureChecker.checkSignature(accountID, signers, 0);
}

LedgerTxnEntry
TransactionFrame::loadSourceAccount(AbstractLedgerTxn& ltx,
                                    LedgerTxnHeader const& header)
{
    auto res = loadAccount(ltx, header, getSourceID());
    if (header.current().ledgerVersion < 8)
    {
        // this is buggy caching that existed in old versions of the protocol
        if (res)
        {
            auto newest = ltx.getNewestVersion(LedgerEntryKey(res.current()));
            mCachedAccount = newest;
        }
        else
        {
            mCachedAccount.reset();
        }
    }
    return res;
}

LedgerTxnEntry
TransactionFrame::loadAccount(AbstractLedgerTxn& ltx,
                              LedgerTxnHeader const& header,
                              AccountID const& accountID)
{
    if (header.current().ledgerVersion < 8 && mCachedAccount &&
        mCachedAccount->data.account().accountID == accountID)
    {
        // this is buggy caching that existed in old versions of the protocol
        auto res = stellar::loadAccount(ltx, accountID);
        if (res)
        {
            res.current() = *mCachedAccount;
        }
        else
        {
            res = ltx.create(*mCachedAccount);
        }

        auto newest = ltx.getNewestVersion(LedgerEntryKey(res.current()));
        mCachedAccount = newest;
        return res;
    }
    else
    {
        return stellar::loadAccount(ltx, accountID);
    }
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
TransactionFrame::commonValidPreSeqNum(Application& app, AbstractLedgerTxn& ltx,
                                       bool forApply)
{
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    if (mOperations.size() == 0)
    {
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

    auto header = ltx.loadHeader();
    if (mEnvelope.tx.timeBounds)
    {
        uint64 closeTime = header.current().scpValue.closeTime;
        if (mEnvelope.tx.timeBounds->minTime > closeTime)
        {
            getResult().result.code(txTOO_EARLY);
            return false;
        }
        if (mEnvelope.tx.timeBounds->maxTime &&
            (mEnvelope.tx.timeBounds->maxTime < closeTime))
        {
            getResult().result.code(txTOO_LATE);
            return false;
        }
    }

    if (mEnvelope.tx.fee < getMinFee(header))
    {
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!loadSourceAccount(ltx, header))
    {
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    return true;
}

void
TransactionFrame::processSeqNum(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    if (header.current().ledgerVersion >= 10)
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        if (sourceAccount.current().data.account().seqNum > mEnvelope.tx.seqNum)
        {
            throw std::runtime_error("unexpected sequence number");
        }
        sourceAccount.current().data.account().seqNum = mEnvelope.tx.seqNum;
    }
}

bool
TransactionFrame::processSignatures(SignatureChecker& signatureChecker,
                                    Application& app,
                                    AbstractLedgerTxn& ltxOuter)
{
    auto allOpsValid = true;
    {
        LedgerTxn ltx(ltxOuter);
        if (ltx.loadHeader().current().ledgerVersion < 10)
        {
            return true;
        }

        for (auto& op : mOperations)
        {
            if (!op->checkSignature(signatureChecker, app, ltx, false))
            {
                allOpsValid = false;
            }
        }
    }

    removeUsedOneTimeSignerKeys(signatureChecker, ltxOuter);

    if (!allOpsValid)
    {
        markResultFailed();
        return false;
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        getResult().result.code(txBAD_AUTH_EXTRA);
        return false;
    }

    return true;
}

TransactionFrame::ValidationType
TransactionFrame::commonValid(SignatureChecker& signatureChecker,
                              Application& app, AbstractLedgerTxn& ltxOuter,
                              SequenceNumber current, bool applying)
{
    LedgerTxn ltx(ltxOuter);
    ValidationType res = ValidationType::kInvalid;

    if (!commonValidPreSeqNum(app, ltx, applying))
    {
        return res;
    }

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    // in older versions, the account's sequence number is updated when taking
    // fees
    if (header.current().ledgerVersion >= 10 || !applying)
    {
        if (current == 0)
        {
            current = sourceAccount.current().data.account().seqNum;
        }
        if (current == INT64_MAX || current + 1 != mEnvelope.tx.seqNum)
        {
            getResult().result.code(txBAD_SEQ);
            return res;
        }
    }

    res = ValidationType::kInvalidUpdateSeqNum;

    if (!checkSignature(
            signatureChecker, sourceAccount,
            sourceAccount.current().data.account().thresholds[THRESHOLD_LOW]))
    {
        getResult().result.code(txBAD_AUTH);
        return res;
    }

    res = ValidationType::kInvalidPostAuth;

    // if we are in applying mode fee was already deduced from signing account
    // balance, if not, we need to check if after that deduction this account
    // will still have minimum balance
    uint32_t feeToPay = (applying && (header.current().ledgerVersion > 8))
                            ? 0
                            : mEnvelope.tx.fee;
    // don't let the account go below the reserve after accounting for
    // liabilities
    if (getAvailableBalance(header, sourceAccount) < feeToPay)
    {
        getResult().result.code(txINSUFFICIENT_BALANCE);
        return res;
    }

    return ValidationType::kFullyValid;
}

void
TransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx)
{
    mCachedAccount.reset();
    resetResults();

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);
    if (!sourceAccount)
    {
        throw std::runtime_error("Unexpected database state");
    }
    auto& acc = sourceAccount.current().data.account();

    int64_t& fee = getResult().feeCharged;
    if (fee > 0)
    {
        fee = std::min(acc.balance, fee);
        // Note: TransactionUtil addBalance checks that reserve plus liabilities
        // are respected. In this case, we allow it to fall below that since it
        // will be caught later in commonValid.
        stellar::addBalance(acc.balance, -fee);
        header.current().feePool += fee;
    }
    // in v10 we update sequence numbers during apply
    if (header.current().ledgerVersion <= 9)
    {
        if (acc.seqNum + 1 != mEnvelope.tx.seqNum)
        {
            // this should not happen as the transaction set is sanitized for
            // sequence numbers
            throw std::runtime_error("Unexpected account state");
        }
        acc.seqNum = mEnvelope.tx.seqNum;
    }
}

void
TransactionFrame::removeUsedOneTimeSignerKeys(
    SignatureChecker& signatureChecker, AbstractLedgerTxn& ltx)
{
    for (auto const& usedAccount : signatureChecker.usedOneTimeSignerKeys())
    {
        removeUsedOneTimeSignerKeys(ltx, usedAccount.first, usedAccount.second);
    }
}

void
TransactionFrame::removeUsedOneTimeSignerKeys(
    AbstractLedgerTxn& ltx, AccountID const& accountID,
    std::set<SignerKey> const& keys) const
{
    auto account = stellar::loadAccount(ltx, accountID);
    if (!account)
    {
        return; // probably account was removed due to merge operation
    }

    auto header = ltx.loadHeader();
    auto changed = std::accumulate(
        std::begin(keys), std::end(keys), false,
        [&](bool r, const SignerKey& signerKey) {
            return r || removeAccountSigner(header, account, signerKey);
        });

    if (changed)
    {
        normalizeSigners(account);
    }
}

bool
TransactionFrame::removeAccountSigner(LedgerTxnHeader const& header,
                                      LedgerTxnEntry& account,
                                      SignerKey const& signerKey) const
{
    auto& acc = account.current().data.account();
    auto it = std::find_if(
        std::begin(acc.signers), std::end(acc.signers),
        [&signerKey](Signer const& signer) { return signer.key == signerKey; });
    if (it != std::end(acc.signers))
    {
        auto removed = stellar::addNumEntries(header, account, -1);
        assert(removed);
        acc.signers.erase(it);
        return true;
    }

    return false;
}

bool
TransactionFrame::checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
                             SequenceNumber current)
{
    mCachedAccount.reset();
    resetResults();

    LedgerTxn ltx(ltxOuter);
    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(), mEnvelope.signatures};
    bool res = commonValid(signatureChecker, app, ltx, current, false) ==
               ValidationType::kFullyValid;
    if (res)
    {
        for (auto& op : mOperations)
        {
            if (!op->checkValid(signatureChecker, app, ltx, false))
            {
                // it's OK to just fast fail here and not try to call
                // checkValid on all operations as the resulting object
                // is only used by applications
                markResultFailed();
                return false;
            }
        }

        if (!signatureChecker.checkAllSignaturesUsed())
        {
            res = false;
            getResult().result.code(txBAD_AUTH_EXTRA);
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
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx)
{
    TransactionMeta tm(1);
    return apply(app, ltx, tm.v1());
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  Application& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaV1& meta)
{
    bool errorEncountered = false;

    // shield outer scope of any side effects with LedgerTxn
    LedgerTxn ltxTx(ltx);
    auto& opTimer = app.getMetrics().NewTimer({"ledger", "operation", "apply"});
    for (auto& op : mOperations)
    {
        auto time = opTimer.TimeScope();
        LedgerTxn ltxOp(ltxTx);
        bool txRes = op->apply(signatureChecker, app, ltxOp);

        if (!txRes)
        {
            errorEncountered = true;
        }
        if (!errorEncountered)
        {
            app.getInvariantManager().checkOnOperationApply(
                op->getOperation(), op->getResult(), ltxOp.getDelta());
        }
        meta.operations.emplace_back(ltxOp.getChanges());
        ltxOp.commit();
    }

    if (!errorEncountered)
    {
        if (ltxTx.loadHeader().current().ledgerVersion < 10)
        {
            if (!signatureChecker.checkAllSignaturesUsed())
            {
                getResult().result.code(txBAD_AUTH_EXTRA);
                // this should never happen: malformed transaction should
                // not be accepted by nodes
                return false;
            }

            // if an error occurred, it is responsibility of account's owner
            // to remove that signer
            removeUsedOneTimeSignerKeys(signatureChecker, ltxTx);
        }

        ltxTx.commit();
    }

    if (errorEncountered)
    {
        meta.operations.clear();
        markResultFailed();
    }
    return !errorEncountered;
}

bool
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                        TransactionMetaV1& meta)
{
    mCachedAccount.reset();
    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(), mEnvelope.signatures};

    bool valid = false;
    {
        LedgerTxn ltxTx(ltx);
        // when applying, a failure during tx validation means that
        // we'll skip trying to apply operations but we'll still
        // process the sequence number if needed
        auto cv = commonValid(signatureChecker, app, ltxTx, 0, true);
        if (cv >= ValidationType::kInvalidUpdateSeqNum)
        {
            processSeqNum(ltxTx);
        }
        auto signaturesValid = cv >= (ValidationType::kInvalidPostAuth) &&
                               processSignatures(signatureChecker, app, ltxTx);
        meta.txChanges = ltxTx.getChanges();
        ltxTx.commit();
        valid = signaturesValid && (cv == ValidationType::kFullyValid);
    }
    return valid && applyOperations(signatureChecker, app, ltx, meta);
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
TransactionFrame::storeTransaction(Database& db, uint32_t ledgerSeq,
                                   TransactionMeta& tm, int txindex,
                                   TransactionResultSet& resultSet) const
{
    auto txBytes(xdr::xdr_to_opaque(mEnvelope));

    resultSet.results.emplace_back(getResultPair());
    auto txResultBytes(xdr::xdr_to_opaque(resultSet.results.back()));

    std::string txBody;
    txBody = decoder::encode_b64(txBytes);

    std::string txResult;
    txResult = decoder::encode_b64(txResultBytes);

    xdr::opaque_vec<> txMeta(xdr::xdr_to_opaque(tm));

    std::string meta;
    meta = decoder::encode_b64(txMeta);

    string txIDString(binToHex(getContentsHash()));

    auto prep = db.getPreparedStatement(
        "INSERT INTO txhistory "
        "( txid, ledgerseq, txindex,  txbody, txresult, txmeta) VALUES "
        "(:id,  :seq,      :txindex, :txb,   :txres,   :meta)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerSeq));
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
TransactionFrame::storeTransactionFee(Database& db, uint32_t ledgerSeq,
                                      LedgerEntryChanges const& changes,
                                      int txindex) const
{
    xdr::opaque_vec<> txChanges(xdr::xdr_to_opaque(changes));

    std::string txChanges64;
    txChanges64 = decoder::encode_b64(txChanges);

    string txIDString(binToHex(getContentsHash()));

    auto prep = db.getPreparedStatement(
        "INSERT INTO txfeehistory "
        "( txid, ledgerseq, txindex,  txchanges) VALUES "
        "(:id,  :seq,      :txindex, :txchanges)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerSeq));
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
    auto lh = LedgerHeaderUtils::loadBySequence(db, sess, ledgerSeq);
    if (!lh)
    {
        throw std::runtime_error("Could not find ledger");
    }
    txSet.previousLedgerHash() = lh->previousLedgerHash;
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
        decoder::decode_b64(txresult64, result);

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
        decoder::decode_b64(changes64, changesRaw);

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
        decoder::decode_b64(txBody, body);

        std::vector<uint8_t> result;
        decoder::decode_b64(txResult, result);

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
