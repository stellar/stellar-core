// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TransactionSQL.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "herder/TxSetFrame.h"
#include "history/CheckpointBuilder.h"
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
    TransactionHistoryResultEntry& results,
    CheckpointBuilder& checkpointBuilder)
{
    ZoneScoped;
    // prepare the txset for saving
    auto lh = LedgerHeaderUtils::loadBySequence(db, sess, ledgerSeq);
    if (!lh)
    {
        throw std::runtime_error("Could not find ledger");
    }
    auto txSet =
        TxSetXDRFrame::makeFromHistoryTransactions(lh->previousLedgerHash, txs);
    TransactionHistoryEntry hist;
    hist.ledgerSeq = ledgerSeq;
    txSet->toXDR(hist.txSet);
    checkpointBuilder.appendTransactionSet(ledgerSeq, hist, results.txResultSet,
                                           /* skipStartupCheck */ true);
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
writeGeneralizedTxSetToStream(uint32 ledgerSeq,
                              std::vector<uint8_t> const& encodedTxSet,
                              std::vector<TransactionFrameBasePtr> const& txs,
                              TransactionHistoryResultEntry& results,
                              CheckpointBuilder& checkpointBuilder)
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

    checkpointBuilder.appendTransactionSet(ledgerSeq, hist, results.txResultSet,
                                           /* skipStartupCheck */ true);
}

void
writeTxSetToStream(
    Database& db, soci::session& sess, uint32 ledgerSeq,
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> const& encodedTxSets,
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>>::const_iterator&
        encodedTxSetIt,
    std::vector<TransactionFrameBasePtr> const& txs,
    TransactionHistoryResultEntry& results,
    CheckpointBuilder& checkpointBuilder)
{
    ZoneScoped;

    // encodedTxSets may *only* contain generalized tx sets, so if the requested
    // ledger is before the first generalized tx set ledger, then we still need
    // to emit the legacy tx set.
    // Starting with the first generalized tx set, all the tx sets will be
    // considered generalized.
    if (encodedTxSets.empty() || ledgerSeq < encodedTxSets.front().first)
    {
        writeNonGeneralizedTxSetToStream(db, sess, ledgerSeq, txs, results,
                                         checkpointBuilder);
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
        writeGeneralizedTxSetToStream(ledgerSeq, encodedTxSetIt->second, txs,
                                      results, checkpointBuilder);
        ++encodedTxSetIt;
    }
}

} // namespace

size_t
populateCheckpointFilesFromDB(Application& app, soci::session& sess,
                              uint32_t ledgerSeq, uint32_t ledgerCount,
                              CheckpointBuilder& checkpointBuilder)
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
                               encodedTxSetIt, txs, results, checkpointBuilder);
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
                           encodedTxSetIt, txs, results, checkpointBuilder);
    }
    return n;
}

void
dropSupportTransactionFeeHistory(Database& db)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    db.getRawSession() << "DROP TABLE IF EXISTS txfeehistory";
}

void
dropSupportTxSetHistory(Database& db)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    db.getRawSession() << "DROP TABLE IF EXISTS txsethistory";
}

void
dropSupportTxHistory(Database& db)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    db.getRawSession() << "DROP TABLE IF EXISTS txhistory";
}
}
