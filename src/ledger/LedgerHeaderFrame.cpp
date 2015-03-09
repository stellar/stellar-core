// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LedgerHeaderFrame.h"
#include "LedgerMaster.h"
#include "lib/json/json.h"
#include "util/XDRStream.h"
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

    LedgerHeaderFrame::LedgerHeaderFrame(LedgerHeaderHistoryEntry const& lastClosed) : mHeader(lastClosed.header)
    {
        if (getHash() != lastClosed.hash)
        {
            throw std::invalid_argument("provided ledger header is invalid");
        }
        mHeader.ledgerSeq++;
        mHeader.previousLedgerHash = lastClosed.hash;
        mHash.fill(0);
    }

    Hash const& LedgerHeaderFrame::getHash()
    {
        if (isZero(mHash))
        {
            // Hash is hash(header-with-hash-field-all-zero)
            mHash = sha256(xdr::xdr_to_msg(mHeader));
            assert(!isZero(mHash));
        }
        return mHash;
    }

    void LedgerHeaderFrame::storeInsert(LedgerMaster& ledgerMaster)
    {
        getHash();

        string hash(binToHex(mHash)), prevHash(binToHex(mHeader.previousLedgerHash)),
            clfHash(binToHex(mHeader.clfHash));

        auto headerBytes(xdr::xdr_to_opaque(mHeader));

        std::string headerEncoded = base64::encode(
            reinterpret_cast<const unsigned char *>(headerBytes.data()),
            headerBytes.size());

        auto& db = ledgerMaster.getDatabase();

        // note: columns other than "data" are there to faciliate lookup/processing
        soci::statement st = (db.getSession().prepare <<
            "INSERT INTO LedgerHeaders (ledgerHash,prevHash,clfHash, "\
            "ledgerSeq,closeTime,data) VALUES"\
            "(:h,:ph,:clf,"\
            ":seq,:ct,:data)",
            use(hash), use(prevHash), use(clfHash),
            use(mHeader.ledgerSeq), use(mHeader.closeTime), use(headerEncoded));
        {
            auto timer = db.getInsertTimer("ledger-header");
            st.execute(true);
        }
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    LedgerHeaderFrame::pointer LedgerHeaderFrame::decodeFromData(std::string const& data)
    {
        LedgerHeader lh;
        string decoded(base64::decode(data));

        xdr::xdr_get g(decoded.c_str(), decoded.c_str() + decoded.length());
        xdr::xdr_argpack_archive(g, lh);
        g.done();

        return make_shared<LedgerHeaderFrame>(lh);
    }

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadByHash(Hash const& hash, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        string hash_s(binToHex(hash));
        string headerEncoded;
        {
            auto& db = ledgerMaster.getDatabase();
            auto timer = db.getSelectTimer("ledger-header");
            db.getSession() <<
                "SELECT data FROM LedgerHeaders "\
                "WHERE ledgerHash = :h",
                into(headerEncoded),
                use(hash_s);
        }
        if (ledgerMaster.getDatabase().getSession().got_data())
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

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadBySequence(uint64_t seq, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        string headerEncoded;
        auto& db = ledgerMaster.getDatabase();

        {
            auto timer = db.getSelectTimer("ledger-header");
            db.getSession() <<
            "SELECT data FROM LedgerHeaders "\
            "WHERE ledgerSeq = :s",
            into(headerEncoded),
            use(seq);
        }
        if (ledgerMaster.getDatabase().getSession().got_data())
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

    size_t LedgerHeaderFrame::copyLedgerHeadersToStream(Database& db,
                                                        soci::session& sess,
                                                        uint64_t ledgerSeq,
                                                        uint64_t ledgerCount,
                                                        XDROutputFileStream& out)
    {
        auto timer = db.getSelectTimer("ledger-header-history");
        uint64_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
        size_t n = 0;

        string headerEncoded;

        assert(begin <= end);

        soci::statement st =
            (sess.prepare <<
             "SELECT data FROM LedgerHeaders "
             "WHERE ledgerSeq >= :begin AND ledgerSeq < :end",
             into(headerEncoded),
             use(begin), use(end));

        st.execute(true);
        while (st.got_data())
        {
            LedgerHeaderHistoryEntry lhe;
            LedgerHeaderFrame::pointer lhf = decodeFromData(headerEncoded);
            lhe.hash = lhf->getHash();
            lhe.header = lhf->mHeader;

            out.writeOne(lhe);
            ++n;
            st.fetch();
        }
        return n;
    }


    void LedgerHeaderFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS LedgerHeaders;";

        db.getSession() <<
            "CREATE TABLE LedgerHeaders ("
            "ledgerHash      CHARACTER(64) PRIMARY KEY,"
            "prevHash        CHARACTER(64) NOT NULL,"
            "clfHash         CHARACTER(64) NOT NULL,"
            "ledgerSeq       BIGINT UNIQUE CHECK (ledgerSeq >= 0),"
            "closeTime       BIGINT NOT NULL CHECK (closeTime >= 0),"
            "data            TEXT NOT NULL"
            ");";

        db.getSession() <<
            "CREATE INDEX LedgersBySeq ON LedgerHeaders ( ledgerSeq );";
    }
}

