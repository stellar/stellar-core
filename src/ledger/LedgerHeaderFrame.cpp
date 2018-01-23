// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "LedgerHeaderFrame.h"
#include "LedgerManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "util/format.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include <util/basen.h>

namespace stellar
{

using namespace soci;
using namespace std;

LedgerHeaderFrame::LedgerHeaderFrame(LedgerHeader const& lh) : mHeader(lh)
{
    mHash.fill(0);
}

LedgerHeaderFrame::LedgerHeaderFrame(LedgerHeaderHistoryEntry const& lastClosed)
    : mHeader(lastClosed.header)
{
    if (getHash() != lastClosed.hash)
    {
        throw std::invalid_argument("provided ledger header is invalid");
    }
    mHeader.ledgerSeq++;
    mHeader.previousLedgerHash = lastClosed.hash;
    mHash.fill(0);
}

bool
LedgerHeaderFrame::isValid(LedgerHeader const& lh)
{
    bool res = (lh.ledgerSeq <= INT32_MAX);

    res = res && (lh.scpValue.closeTime <= INT64_MAX);
    res = res && (lh.feePool >= 0);
    res = res && (lh.idPool <= INT64_MAX);
    return res;
}

Hash const&
LedgerHeaderFrame::getHash() const
{
    if (isZero(mHash))
    {
        mHash = sha256(xdr::xdr_to_opaque(mHeader));
        assert(!isZero(mHash));
    }
    return mHash;
}

SequenceNumber
LedgerHeaderFrame::getStartingSequenceNumber() const
{
    return static_cast<uint64_t>(mHeader.ledgerSeq) << 32;
}

uint64_t
LedgerHeaderFrame::getLastGeneratedID() const
{
    return mHeader.idPool;
}

uint64_t
LedgerHeaderFrame::generateID()
{
    return ++mHeader.idPool;
}

void
LedgerHeaderFrame::storeInsert(LedgerManager& ledgerManager) const
{
    if (!isValid(mHeader))
    {
        throw std::runtime_error("invalid ledger header (insert)");
    }

    getHash();

    string hash(binToHex(mHash)),
        prevHash(binToHex(mHeader.previousLedgerHash)),
        bucketListHash(binToHex(mHeader.bucketListHash));

    auto headerBytes(xdr::xdr_to_opaque(mHeader));

    std::string headerEncoded;
    headerEncoded = bn::encode_b64(headerBytes);

    auto& db = ledgerManager.getDatabase();

    // note: columns other than "data" are there to faciliate lookup/processing
    auto prep = db.getPreparedStatement(
        "INSERT INTO ledgerheaders "
        "(ledgerhash, prevhash, bucketlisthash, ledgerseq, closetime, data) "
        "VALUES "
        "(:h,        :ph,      :blh,            :seq,     :ct,       :data)");
    auto& st = prep.statement();
    st.exchange(use(hash));
    st.exchange(use(prevHash));
    st.exchange(use(bucketListHash));
    st.exchange(use(mHeader.ledgerSeq));
    st.exchange(use(mHeader.scpValue.closeTime));
    st.exchange(use(headerEncoded));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("ledger-header");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

LedgerHeaderFrame::pointer
LedgerHeaderFrame::decodeFromData(std::string const& data)
{
    LedgerHeader lh;
    std::vector<uint8_t> decoded;
    bn::decode_b64(data, decoded);

    xdr::xdr_get g(&decoded.front(), &decoded.back() + 1);
    xdr::xdr_argpack_archive(g, lh);
    g.done();

    if (!isValid(lh))
    {
        throw std::runtime_error("invalid ledger header (load)");
    }

    return make_shared<LedgerHeaderFrame>(lh);
}

LedgerHeaderFrame::pointer
LedgerHeaderFrame::loadByHash(Hash const& hash, Database& db)
{
    LedgerHeaderFrame::pointer lhf;

    string hash_s(binToHex(hash));
    string headerEncoded;

    auto prep = db.getPreparedStatement("SELECT data FROM ledgerheaders "
                                        "WHERE ledgerhash = :h");
    auto& st = prep.statement();
    st.exchange(into(headerEncoded));
    st.exchange(use(hash_s));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("ledger-header");
        st.execute(true);
    }
    if (st.got_data())
    {
        lhf = decodeFromData(headerEncoded);
        if (lhf->getHash() != hash)
        {
            // wrong hash
            lhf.reset();
        }
    }

    return lhf;
}

LedgerHeaderFrame::pointer
LedgerHeaderFrame::loadBySequence(uint32_t seq, Database& db,
                                  soci::session& sess)
{
    LedgerHeaderFrame::pointer lhf;

    string headerEncoded;
    {
        auto timer = db.getSelectTimer("ledger-header");
        sess << "SELECT data FROM ledgerheaders "
                "WHERE ledgerseq = :s",
            into(headerEncoded), use(seq);
    }
    if (sess.got_data())
    {
        lhf = decodeFromData(headerEncoded);
        uint32_t loadedSeq = lhf->mHeader.ledgerSeq;

        if (loadedSeq != seq)
        {
            throw std::runtime_error(
                fmt::format("Wrong sequence number in ledger header database: "
                            "loaded ledger {} contains {}",
                            seq, loadedSeq));
        }
    }

    return lhf;
}

size_t
LedgerHeaderFrame::copyLedgerHeadersToStream(Database& db, soci::session& sess,
                                             uint32_t ledgerSeq,
                                             uint32_t ledgerCount,
                                             XDROutputFileStream& headersOut)
{
    auto timer = db.getSelectTimer("ledger-header-history");
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    string headerEncoded;

    assert(begin <= end);

    soci::statement st =
        (sess.prepare << "SELECT data FROM ledgerheaders "
                         "WHERE ledgerseq >= :begin AND ledgerseq < :end ORDER "
                         "BY ledgerseq ASC",
         into(headerEncoded), use(begin), use(end));

    st.execute(true);
    while (st.got_data())
    {
        LedgerHeaderHistoryEntry lhe;
        LedgerHeaderFrame::pointer lhf = decodeFromData(headerEncoded);
        lhe.hash = lhf->getHash();
        lhe.header = lhf->mHeader;
        CLOG(DEBUG, "Ledger")
            << "Streaming ledger-header " << lhe.header.ledgerSeq;
        headersOut.writeOne(lhe);
        ++n;
        st.fetch();
    }
    return n;
}

void
LedgerHeaderFrame::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    db.getSession() << "DELETE FROM ledgerheaders WHERE ledgerseq IN (SELECT "
                       "ledgerseq FROM ledgerheaders WHERE ledgerseq <= "
                    << ledgerSeq << " LIMIT " << count << ")";
}

void
LedgerHeaderFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS ledgerheaders;";

    db.getSession() << "CREATE TABLE ledgerheaders ("
                       "ledgerhash      CHARACTER(64) PRIMARY KEY,"
                       "prevhash        CHARACTER(64) NOT NULL,"
                       "bucketlisthash  CHARACTER(64) NOT NULL,"
                       "ledgerseq       INT UNIQUE CHECK (ledgerseq >= 0),"
                       "closetime       BIGINT NOT NULL CHECK (closetime >= 0),"
                       "data            TEXT NOT NULL"
                       ");";

    db.getSession()
        << "CREATE INDEX ledgersbyseq ON ledgerheaders ( ledgerseq );";
}
}
