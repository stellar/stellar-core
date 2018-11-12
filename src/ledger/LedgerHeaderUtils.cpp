// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHeaderUtils.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "util/Decoder.h"
#include "util/XDRStream.h"
#include "util/format.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include <util/basen.h>

#include "util/Logging.h"

namespace stellar
{

namespace LedgerHeaderUtils
{

bool
isValid(LedgerHeader const& lh)
{
    bool res = (lh.ledgerSeq <= INT32_MAX);

    res = res && (lh.scpValue.closeTime <= INT64_MAX);
    res = res && (lh.feePool >= 0);
    res = res && (lh.idPool <= INT64_MAX);
    return res;
}

void
storeInDatabase(Database& db, LedgerHeader const& header)
{
    if (!isValid(header))
    {
        throw std::runtime_error("invalid ledger header (insert)");
    }

    auto headerBytes(xdr::xdr_to_opaque(header));
    std::string hash(binToHex(sha256(headerBytes))),
        prevHash(binToHex(header.previousLedgerHash)),
        bucketListHash(binToHex(header.bucketListHash));

    std::string headerEncoded;
    headerEncoded = decoder::encode_b64(headerBytes);

    // note: columns other than "data" are there to faciliate lookup/processing
    auto prep = db.getPreparedStatement(
        "INSERT INTO ledgerheaders "
        "(ledgerhash, prevhash, bucketlisthash, ledgerseq, closetime, data) "
        "VALUES "
        "(:h,        :ph,      :blh,            :seq,     :ct,       :data)");
    auto& st = prep.statement();
    st.exchange(soci::use(hash));
    st.exchange(soci::use(prevHash));
    st.exchange(soci::use(bucketListHash));
    st.exchange(soci::use(header.ledgerSeq));
    st.exchange(soci::use(header.scpValue.closeTime));
    st.exchange(soci::use(headerEncoded));
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

LedgerHeader
decodeFromData(std::string const& data)
{
    LedgerHeader lh;
    std::vector<uint8_t> decoded;
    decoder::decode_b64(data, decoded);

    xdr::xdr_get g(&decoded.front(), &decoded.back() + 1);
    xdr::xdr_argpack_archive(g, lh);
    g.done();

    if (!isValid(lh))
    {
        throw std::runtime_error("invalid ledger header (load)");
    }
    return lh;
}

std::shared_ptr<LedgerHeader>
loadByHash(Database& db, Hash const& hash)
{
    std::shared_ptr<LedgerHeader> lhPtr;

    std::string hash_s(binToHex(hash));
    std::string headerEncoded;

    auto prep = db.getPreparedStatement("SELECT data FROM ledgerheaders "
                                        "WHERE ledgerhash = :h");
    auto& st = prep.statement();
    st.exchange(soci::into(headerEncoded));
    st.exchange(soci::use(hash_s));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("ledger-header");
        st.execute(true);
    }
    if (st.got_data())
    {
        auto lh = decodeFromData(headerEncoded);
        lhPtr = std::make_shared<LedgerHeader>(lh);
        auto ledgerHash = sha256(xdr::xdr_to_opaque(lh));
        if (ledgerHash != hash)
        {
            throw std::runtime_error(
                fmt::format("Wrong hash in ledger header database: "
                            "loaded ledger {} contains {}",
                            binToHex(ledgerHash), binToHex(hash)));
        }
    }

    return lhPtr;
}

std::shared_ptr<LedgerHeader>
loadBySequence(Database& db, soci::session& sess, uint32_t seq)
{
    std::shared_ptr<LedgerHeader> lhPtr;

    std::string headerEncoded;
    {
        auto timer = db.getSelectTimer("ledger-header");
        sess << "SELECT data FROM ledgerheaders "
                "WHERE ledgerseq = :s",
            soci::into(headerEncoded), soci::use(seq);
    }
    if (sess.got_data())
    {
        auto lh = decodeFromData(headerEncoded);
        lhPtr = std::make_shared<LedgerHeader>(lh);

        if (lh.ledgerSeq != seq)
        {
            throw std::runtime_error(
                fmt::format("Wrong sequence number in ledger header database: "
                            "loaded ledger {} contains {}",
                            seq, lh.ledgerSeq));
        }
    }

    return lhPtr;
}

void
deleteOldEntries(Database& db, uint32_t ledgerSeq, uint32_t count)
{
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "ledgerheaders", "ledgerseq");
}

size_t
copyToStream(Database& db, soci::session& sess, uint32_t ledgerSeq,
             uint32_t ledgerCount, XDROutputFileStream& headersOut)
{
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    assert(begin <= end);

    std::string headerEncoded;

    auto timer = db.getSelectTimer("ledger-header-history");
    soci::statement st =
        (sess.prepare << "SELECT data FROM ledgerheaders "
                         "WHERE ledgerseq >= :begin AND ledgerseq < :end ORDER "
                         "BY ledgerseq ASC",
         soci::into(headerEncoded), soci::use(begin), soci::use(end));

    size_t n = 0;
    st.execute(true);
    while (st.got_data())
    {
        LedgerHeaderHistoryEntry lhe;
        lhe.header = decodeFromData(headerEncoded);
        lhe.hash = sha256(xdr::xdr_to_opaque(lhe.header));
        CLOG(DEBUG, "Ledger")
            << "Streaming ledger-header " << lhe.header.ledgerSeq;
        headersOut.writeOne(lhe);
        ++n;
        st.fetch();
    }
    return n;
}

void
dropAll(Database& db)
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
}
