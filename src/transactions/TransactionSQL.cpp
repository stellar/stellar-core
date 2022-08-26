// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionSQL.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerHeaderUtils.h"
#include "main/Application.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h"
#include "xdrpp/marshal.h"
#include "xdrpp/message.h"
#include <Tracy.hpp>

namespace stellar
{
namespace
{
using namespace xdr;
// XDR marshaller that replaces all the transaction envelopes with their full
// hashes. This is needed because we store the transaction envelopes separately
// and use for a different purpose, so trimming them from
// GeneralizedTransactionSet allows to avoid duplication.
class GeneralizedTxSetPacker
{
  public:
    GeneralizedTxSetPacker(GeneralizedTransactionSet const& xdrTxSet)
        : mBuffer(xdr_argpack_size(xdrTxSet))
    {
    }

    std::vector<uint8_t> const&
    getResult()
    {
        mBuffer.resize(mCurrIndex);
        return mBuffer;
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        putBytes(&t, 4);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        putBytes(&t, 8);
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T const& t)
    {
        if (xdr_traits<T>::variable_nelem)
        {
            uint32_t size = size32(t.size());
            putBytes(&size, 4);
        }
        putBytes(t.data(), t.size());
    }

    template <typename T>
    typename std::enable_if<!std::is_same<TransactionEnvelope, T>::value &&
                            (xdr_traits<T>::is_class ||
                             xdr_traits<T>::is_container)>::type
    operator()(T const& t)
    {
        xdr_traits<T>::save(*this, t);
    }

    template <typename T>
    typename std::enable_if<std::is_same<TransactionEnvelope, T>::value>::type
    operator()(T const& tx)
    {
        Hash hash = xdrSha256(tx);
        putBytes(hash.data(), hash.size());
    }

  private:
    // While unlikely, there is a possible edge case where `sum(hash sizes) >
    // sum(TransactionEnvelope sizes)`. Hence instead of a strict size check we
    // extend the buffer.
    void
    maybeExtend(size_t byBytes)
    {
        if (mCurrIndex + byBytes > mBuffer.size())
        {
            mBuffer.resize(mCurrIndex + byBytes);
        }
    }

    void
    putBytes(void const* buf, size_t len)
    {
        if (len == 0)
        {
            return;
        }
        maybeExtend(len);
        std::memcpy(mBuffer.data() + mCurrIndex, buf, len);
        mCurrIndex += len;
    }

    std::vector<uint8_t> mBuffer;
    size_t mCurrIndex = 0;
};

// XDR un-marshaller that reconstructs the GeneralizedTxSet using the trimmed
// result of GeneralizedTxSetPacker and the actual TransactionEnvelopes.
class GeneralizedTxSetUnpacker
{
  public:
    GeneralizedTxSetUnpacker(
        std::vector<uint8_t> const& buffer,
        UnorderedMap<Hash, TransactionFrameBase const*> const& txs)
        : mBuffer(buffer), mTxs(txs)
    {
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        uint32_t v;
        getBytes(&v, 4);
        t = xdr_traits<T>::from_uint(v);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        uint64_t v;
        getBytes(&v, 8);
        t = xdr_traits<T>::from_uint(v);
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T& t)
    {
        if (xdr_traits<T>::variable_nelem)
        {
            std::uint32_t size;
            getBytes(&size, 4);
            t.resize(size);
        }
        getBytes(t.data(), t.size());
    }

    template <typename T>
    typename std::enable_if<!std::is_same<TransactionEnvelope, T>::value &&
                            (xdr_traits<T>::is_class ||
                             xdr_traits<T>::is_container)>::type
    operator()(T& t)
    {
        xdr_traits<T>::load(*this, t);
    }

    template <typename T>
    typename std::enable_if<std::is_same<TransactionEnvelope, T>::value>::type
    operator()(T& tx)
    {
        Hash hash;
        getBytes(hash.data(), hash.size());
        auto it = mTxs.find(hash);
        if (it == mTxs.end())
        {
            throw std::invalid_argument(
                "Could not find transaction corresponding to hash.");
        }
        tx = it->second->getEnvelope();
    }

  private:
    void
    check(std::size_t n) const
    {
        if (mCurrIndex + n > mBuffer.size())
        {
            throw xdr_overflow(
                "Input buffer space overflow in GeneralizedTxSetUnpacker.");
        }
    }

    void
    getBytes(void* buf, size_t len)
    {
        if (len != 0)
        {
            check(len);
            std::memcpy(buf, mBuffer.data() + mCurrIndex, len);
            mCurrIndex += len;
        }
    }

    std::vector<uint8_t> const& mBuffer;
    UnorderedMap<Hash, TransactionFrameBase const*> const& mTxs;
    size_t mCurrIndex = 0;
};

std::vector<std::pair<uint32_t, std::vector<uint8_t>>>
readEncodedTxSets(Application& app, soci::session& sess, uint32_t ledgerSeq,
                  uint32_t ledgerCount)
{
    ZoneScoped;
    auto& db = app.getDatabase();
    auto timer = db.getSelectTimer("txsethistory");
    std::string base64TxSet;
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    uint32_t curLedgerSeq;
    releaseAssert(begin <= end);
    soci::statement st =
        (sess.prepare << "SELECT ledgerseq, txset FROM txsethistory "
                         "WHERE ledgerseq >= :begin AND ledgerseq < :end ORDER "
                         "BY ledgerseq ASC",
         soci::into(curLedgerSeq), soci::into(base64TxSet), soci::use(begin),
         soci::use(end));

    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> txSets;

    st.execute(true);

    while (st.got_data())
    {
        auto& [txSetLedgerSeq, encodedTxSet] = txSets.emplace_back();
        txSetLedgerSeq = curLedgerSeq;
        decoder::decode_b64(base64TxSet, encodedTxSet);
        st.fetch();
    }
    return txSets;
}

void
writeNonGeneralizedTxSetToStream(
    Database& db, soci::session& sess, uint32 ledgerSeq,
    std::vector<TransactionFrameBasePtr> const& txs,
    TransactionHistoryResultEntry& results, XDROutputFileStream& txOut,
    XDROutputFileStream& txResultOut)
{
    ZoneScoped;
    // prepare the txset for saving
    auto lh = LedgerHeaderUtils::loadBySequence(db, sess, ledgerSeq);
    if (!lh)
    {
        throw std::runtime_error("Could not find ledger");
    }
    auto txSet =
        TxSetFrame::makeFromHistoryTransactions(lh->previousLedgerHash, txs);
    TransactionHistoryEntry hist;
    hist.ledgerSeq = ledgerSeq;
    txSet->toXDR(hist.txSet);
    txOut.writeOne(hist);
    txResultOut.writeOne(results);
}

void
checkEncodedGeneralizedTxSetIsEmpty(std::vector<uint8_t> const& encodedTxSet)
{
    ZoneScoped;
    UnorderedMap<Hash, TransactionFrameBase const*> txByHash;
    GeneralizedTxSetUnpacker unpacker(encodedTxSet, txByHash);
    GeneralizedTransactionSet txSet;
    // Unpacker throws when transaction envelope is not found, hence this only
    // doesn't throw for empty tx sets.
    xdr_argpack_archive(unpacker, txSet);
}

void
writeGeneralizedTxSetToStream(Database& db, soci::session& sess,
                              uint32 ledgerSeq,
                              std::vector<uint8_t> const& encodedTxSet,
                              std::vector<TransactionFrameBasePtr> const& txs,
                              TransactionHistoryResultEntry& results,
                              XDROutputFileStream& txOut,
                              XDROutputFileStream& txResultOut)
{
    ZoneScoped;
    UnorderedMap<Hash, TransactionFrameBase const*> txByHash;
    for (auto const& tx : txs)
    {
        txByHash[tx->getFullHash()] = tx.get();
    }

    GeneralizedTxSetUnpacker unpacker(encodedTxSet, txByHash);
    TransactionHistoryEntry hist;
    hist.ledgerSeq = ledgerSeq;
    hist.ext.v(1);
    xdr_argpack_archive(unpacker, hist.ext.generalizedTxSet());

    txOut.writeOne(hist);
    txResultOut.writeOne(results);
}

void
writeTxSetToStream(
    Database& db, soci::session& sess, uint32 ledgerSeq,
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> const& encodedTxSets,
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>>::const_iterator&
        encodedTxSetIt,
    std::vector<TransactionFrameBasePtr> const& txs,
    TransactionHistoryResultEntry& results, XDROutputFileStream& txOut,
    XDROutputFileStream& txResultOut)
{
    // encodedTxSets may *only* contain generalized tx sets, so if the requested
    // ledger is before the first generalized tx set ledger, then we still need
    // to emit the legacy tx set.
    // Starting with the first generalized tx set, all the tx sets will be
    // considered generalized.
    if (encodedTxSets.empty() || ledgerSeq < encodedTxSets.front().first)
    {
        writeNonGeneralizedTxSetToStream(db, sess, ledgerSeq, txs, results,
                                         txOut, txResultOut);
    }
    else
    {
        // Currently there can be gaps in this method calls when there are no
        // transactions in the set.
        while (encodedTxSetIt != encodedTxSets.end() &&
               ledgerSeq != encodedTxSetIt->first)
        {
            checkEncodedGeneralizedTxSetIsEmpty(encodedTxSetIt->second);
            ++encodedTxSetIt;
        }
        if (encodedTxSetIt == encodedTxSets.end())
        {
            throw std::runtime_error(
                "Could not find tx set corresponding to the ledger.");
        }
        writeGeneralizedTxSetToStream(db, sess, ledgerSeq,
                                      encodedTxSetIt->second, txs, results,
                                      txOut, txResultOut);
        ++encodedTxSetIt;
    }
}

} // namespace

void
storeTransaction(Database& db, uint32_t ledgerSeq,
                 TransactionFrameBasePtr const& tx, TransactionMeta const& tm,
                 TransactionResultSet const& resultSet)
{
    ZoneScoped;
    std::string txBody =
        decoder::encode_b64(xdr::xdr_to_opaque(tx->getEnvelope()));
    std::string txResult =
        decoder::encode_b64(xdr::xdr_to_opaque(resultSet.results.back()));
    std::string meta = decoder::encode_b64(xdr::xdr_to_opaque(tm));

    std::string txIDString = binToHex(tx->getContentsHash());
    uint32_t txIndex = static_cast<uint32_t>(resultSet.results.size());

    auto prep = db.getPreparedStatement(
        "INSERT INTO txhistory "
        "( txid, ledgerseq, txindex,  txbody, txresult, txmeta) VALUES "
        "(:id,  :seq,      :txindex, :txb,   :txres,   :meta)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::use(txIndex));
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
storeTxSet(Database& db, uint32_t ledgerSeq, TxSetFrame const& txSet)
{
    ZoneScoped;
    if (!txSet.isGeneralizedTxSet())
    {
        return;
    }
    GeneralizedTransactionSet xdrTxSet;
    txSet.toXDR(xdrTxSet);
    GeneralizedTxSetPacker txSetPacker(xdrTxSet);
    xdr::xdr_argpack_archive(txSetPacker, xdrTxSet);
    std::string trimmedTxSet = decoder::encode_b64(txSetPacker.getResult());

    auto prep = db.getPreparedStatement("INSERT INTO txsethistory "
                                        "( ledgerseq, txset) VALUES "
                                        "(:seq, :txset)");

    auto& st = prep.statement();
    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::use(trimmedTxSet));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("txsethistory");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
storeTransactionFee(Database& db, uint32_t ledgerSeq,
                    TransactionFrameBasePtr const& tx,
                    LedgerEntryChanges const& changes, uint32_t txIndex)
{
    ZoneScoped;
    std::string txChanges = decoder::encode_b64(xdr::xdr_to_opaque(changes));

    std::string txIDString = binToHex(tx->getContentsHash());

    auto prep = db.getPreparedStatement(
        "INSERT INTO txfeehistory "
        "( txid, ledgerseq, txindex,  txchanges) VALUES "
        "(:id,  :seq,      :txindex, :txchanges)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::use(txIndex));
    st.exchange(soci::use(txChanges));
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

TransactionResultSet
getTransactionHistoryResults(Database& db, uint32 ledgerSeq)
{
    ZoneScoped;
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
getTransactionFeeMeta(Database& db, uint32 ledgerSeq)
{
    ZoneScoped;
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
copyTransactionsToStream(Application& app, soci::session& sess,
                         uint32_t ledgerSeq, uint32_t ledgerCount,
                         XDROutputFileStream& txOut,
                         XDROutputFileStream& txResultOut)
{
    ZoneScoped;

    auto encodedTxSets = readEncodedTxSets(app, sess, ledgerSeq, ledgerCount);
    auto encodedTxSetIt = encodedTxSets.cbegin();

    auto& db = app.getDatabase();
    auto timer = db.getSelectTimer("txhistory");
    std::string txBody, txResult, txMeta;
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    TransactionEnvelope tx;
    uint32_t curLedgerSeq;

    releaseAssert(begin <= end);
    soci::statement st =
        (sess.prepare << "SELECT ledgerseq, txbody, txresult FROM txhistory "
                         "WHERE ledgerseq >= :begin AND ledgerseq < :end ORDER "
                         "BY ledgerseq ASC, txindex ASC",
         soci::into(curLedgerSeq), soci::into(txBody), soci::into(txResult),
         soci::use(begin), soci::use(end));

    auto lh = LedgerHeaderUtils::loadBySequence(db, sess, ledgerSeq);
    if (!lh)
    {
        throw std::runtime_error("Could not find ledger");
    }

    Hash h;
    std::vector<TransactionFrameBasePtr> txs;
    TransactionHistoryResultEntry results;

    st.execute(true);

    uint32_t lastLedgerSeq = curLedgerSeq;
    results.ledgerSeq = curLedgerSeq;

    while (st.got_data())
    {
        if (curLedgerSeq != lastLedgerSeq)
        {
            writeTxSetToStream(db, sess, lastLedgerSeq, encodedTxSets,
                               encodedTxSetIt, txs, results, txOut,
                               txResultOut);
            // reset state
            txs.clear();
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
        auto txFrame = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), tx);
        txs.emplace_back(txFrame);

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
        writeTxSetToStream(db, sess, lastLedgerSeq, encodedTxSets,
                           encodedTxSetIt, txs, results, txOut, txResultOut);
    }
    return n;
}

void
createTxSetHistoryTable(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS txsethistory";
    db.getSession() << "CREATE TABLE txsethistory ("
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "txset       TEXT NOT NULL,"
                       "PRIMARY KEY (ledgerseq)"
                       ")";
}

void
dropTransactionHistory(Database& db)
{
    ZoneScoped;
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

    createTxSetHistoryTable(db);
}

void
deleteOldTransactionHistoryEntries(Database& db, uint32_t ledgerSeq,
                                   uint32_t count)
{
    ZoneScoped;
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "txhistory", "ledgerseq");
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "txsethistory", "ledgerseq");
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "txfeehistory", "ledgerseq");
}

void
deleteNewerTransactionHistoryEntries(Database& db, uint32_t ledgerSeq)
{
    ZoneScoped;
    DatabaseUtils::deleteNewerEntriesHelper(db.getSession(), ledgerSeq,
                                            "txhistory", "ledgerseq");
    DatabaseUtils::deleteNewerEntriesHelper(db.getSession(), ledgerSeq,
                                            "txsethistory", "ledgerseq");
    DatabaseUtils::deleteNewerEntriesHelper(db.getSession(), ledgerSeq,
                                            "txfeehistory", "ledgerseq");
}

}
