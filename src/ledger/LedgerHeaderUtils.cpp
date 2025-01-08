// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHeaderUtils.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "history/CheckpointBuilder.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <fmt/format.h>
#include <util/basen.h>

namespace stellar
{

namespace LedgerHeaderUtils
{

uint32_t
getFlags(LedgerHeader const& lh)
{
    return lh.ext.v() == 1 ? lh.ext.v1().flags : 0;
}

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
storeInDatabase(Database& db, LedgerHeader const& header, SessionWrapper& sess)
{
    ZoneScoped;
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

    // note: columns other than "data" are there to facilitate lookup/processing
    auto prep = db.getPreparedStatement(
        "INSERT INTO ledgerheaders "
        "(ledgerhash, prevhash, bucketlisthash, ledgerseq, closetime, data) "
        "VALUES "
        "(:h,        :ph,      :blh,            :seq,     :ct,       :data)",
        sess);
    auto& st = prep.statement();
    st.exchange(soci::use(hash));
    st.exchange(soci::use(prevHash));
    st.exchange(soci::use(bucketListHash));
    st.exchange(soci::use(header.ledgerSeq));
    st.exchange(soci::use(header.scpValue.closeTime));
    st.exchange(soci::use(headerEncoded));
    st.define_and_bind();
    {
        ZoneNamedN(insertLedgerHeadersZone, "insert ledgerheaders", true);
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
    ZoneScoped;
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
    ZoneScoped;
    std::shared_ptr<LedgerHeader> lhPtr;

    std::string hash_s(binToHex(hash));
    std::string headerEncoded;

    auto prep = db.getPreparedStatement("SELECT data FROM ledgerheaders "
                                        "WHERE ledgerhash = :h",
                                        db.getSession());
    auto& st = prep.statement();
    st.exchange(soci::into(headerEncoded));
    st.exchange(soci::use(hash_s));
    st.define_and_bind();
    {
        ZoneNamedN(selectLedgerHeadersZone, "select ledgerheaders", true);
        st.execute(true);
    }
    if (st.got_data())
    {
        auto lh = decodeFromData(headerEncoded);
        lhPtr = std::make_shared<LedgerHeader>(lh);
        auto ledgerHash = xdrSha256(lh);
        if (ledgerHash != hash)
        {
            throw std::runtime_error(
                fmt::format(FMT_STRING("Wrong hash in ledger header database: "
                                       "loaded ledger {} contains {}"),
                            binToHex(ledgerHash), binToHex(hash)));
        }
    }

    return lhPtr;
}

uint32_t
loadMaxLedgerSeq(Database& db)
{
    ZoneScoped;
    uint32_t seq = 0;
    soci::indicator maxIndicator;
    auto prep = db.getPreparedStatement(
        "SELECT MAX(ledgerseq) FROM ledgerheaders", db.getSession());
    auto& st = prep.statement();
    st.exchange(soci::into(seq, maxIndicator));
    st.define_and_bind();
    st.execute(true);
    if (maxIndicator == soci::indicator::i_ok)
    {
        return seq;
    }
    return 0;
}

std::shared_ptr<LedgerHeader>
loadBySequence(Database& db, soci::session& sess, uint32_t seq)
{
    ZoneScoped;
    std::shared_ptr<LedgerHeader> lhPtr;

    std::string headerEncoded;
    {
        ZoneNamedN(selectLedgerHeadersZone, "select ledgerheaders", true);
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
            throw std::runtime_error(fmt::format(
                FMT_STRING("Wrong sequence number in ledger header database: "
                           "loaded ledger {:d} contains {:d}"),
                seq, lh.ledgerSeq));
        }
    }

    return lhPtr;
}

void
deleteOldEntries(soci::session& sess, uint32_t ledgerSeq, uint32_t count)
{
    ZoneScoped;
    DatabaseUtils::deleteOldEntriesHelper(sess, ledgerSeq, count,
                                          "ledgerheaders", "ledgerseq");
}

size_t
copyToStream(Database& db, soci::session& sess, uint32_t ledgerSeq,
             uint32_t ledgerCount, CheckpointBuilder& checkpointBuilder)
{
    ZoneNamedN(selectLedgerHeadersZone, "select ledgerheaders history", true);
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    releaseAssert(begin <= end);

    std::string headerEncoded;

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
        lhe.hash = xdrSha256(lhe.header);
        CLOG_DEBUG(Ledger, "Streaming ledger-header {}", lhe.header.ledgerSeq);
        checkpointBuilder.appendLedgerHeader(lhe.header,
                                             /* skipStartupCheck */ true);
        ++n;
        st.fetch();
    }
    return n;
}

void
dropAll(Database& db)
{
    std::string coll = db.getSimpleCollationClause();

    db.getRawSession() << "DROP TABLE IF EXISTS ledgerheaders;";
    db.getRawSession()
        << "CREATE TABLE ledgerheaders ("
        << "ledgerhash      CHARACTER(64) " << coll << " PRIMARY KEY,"
        << "prevhash        CHARACTER(64) NOT NULL,"
           "bucketlisthash  CHARACTER(64) NOT NULL,"
           "ledgerseq       INT UNIQUE CHECK (ledgerseq >= 0),"
           "closetime       BIGINT NOT NULL CHECK (closetime >= 0),"
           "data            TEXT NOT NULL"
           ");";
    db.getRawSession()
        << "CREATE INDEX ledgersbyseq ON ledgerheaders ( ledgerseq );";
}
}
}
