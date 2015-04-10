// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerHeaderFrame.h"
#include "LedgerManager.h"
#include "util/XDRStream.h"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "xdrpp/marshal.h"
#include "database/Database.h"
#include <cereal/external/base64.hpp>

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
    getHash();

    string hash(binToHex(mHash)),
        prevHash(binToHex(mHeader.previousLedgerHash)),
        bucketListHash(binToHex(mHeader.bucketListHash));

    auto headerBytes(xdr::xdr_to_opaque(mHeader));

    std::string headerEncoded = base64::encode(
        reinterpret_cast<const unsigned char*>(headerBytes.data()),
        headerBytes.size());

    auto& db = ledgerManager.getDatabase();

    // note: columns other than "data" are there to faciliate lookup/processing
    soci::statement st =
        (db.getSession().prepare << "INSERT INTO LedgerHeaders "
                                    "(ledgerHash,prevHash,bucketListHash, "
                                    "ledgerSeq,closeTime,data) VALUES"
                                    "(:h,:ph,:blh,"
                                    ":seq,:ct,:data)",
         use(hash), use(prevHash), use(bucketListHash), use(mHeader.ledgerSeq),
         use(mHeader.closeTime), use(headerEncoded));
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
    string decoded(base64::decode(data));

    xdr::xdr_get g(decoded.c_str(), decoded.c_str() + decoded.length());
    xdr::xdr_argpack_archive(g, lh);
    g.done();

    return make_shared<LedgerHeaderFrame>(lh);
}

LedgerHeaderFrame::pointer
LedgerHeaderFrame::loadByHash(Hash const& hash, Database& db)
{
    LedgerHeaderFrame::pointer lhf;

    string hash_s(binToHex(hash));
    string headerEncoded;
    {
        auto timer = db.getSelectTimer("ledger-header");
        db.getSession() << "SELECT data FROM LedgerHeaders "
                           "WHERE ledgerHash = :h",
            into(headerEncoded), use(hash_s);
    }
    if (db.getSession().got_data())
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
        sess << "SELECT data FROM LedgerHeaders "
                "WHERE ledgerSeq = :s",
            into(headerEncoded), use(seq);
    }
    if (sess.got_data())
    {
        lhf = decodeFromData(headerEncoded);

        if (lhf->mHeader.ledgerSeq != seq)
        {
            // wrong sequence number
            lhf.reset();
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
        (sess.prepare << "SELECT data FROM LedgerHeaders "
                         "WHERE ledgerSeq >= :begin AND ledgerSeq < :end ORDER "
                         "BY ledgerSeq ASC",
         into(headerEncoded), use(begin), use(end));

    st.execute(true);
    while (st.got_data())
    {
        LedgerHeaderHistoryEntry lhe;
        LedgerHeaderFrame::pointer lhf = decodeFromData(headerEncoded);
        lhe.hash = lhf->getHash();
        lhe.header = lhf->mHeader;
        CLOG(DEBUG, "Ledger") << "Streaming ledger-header "
                              << lhe.header.ledgerSeq;
        headersOut.writeOne(lhe);
        ++n;
        st.fetch();
    }
    return n;
}

void
LedgerHeaderFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS LedgerHeaders;";

    db.getSession() << "CREATE TABLE LedgerHeaders ("
                       "ledgerHash      CHARACTER(64) PRIMARY KEY,"
                       "prevHash        CHARACTER(64) NOT NULL,"
                       "bucketListHash  CHARACTER(64) NOT NULL,"
                       "ledgerSeq       INT UNIQUE CHECK (ledgerSeq >= 0),"
                       "closeTime       BIGINT NOT NULL CHECK (closeTime >= 0),"
                       "data            TEXT NOT NULL"
                       ");";

    db.getSession()
        << "CREATE INDEX LedgersBySeq ON LedgerHeaders ( ledgerSeq );";
}
}
