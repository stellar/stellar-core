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
            txSetHash(binToHex(mHeader.txSetHash)), clfHash(binToHex(mHeader.clfHash));

        auto& db = ledgerMaster.getDatabase();
        soci::statement st = (db.getSession().prepare <<
            "INSERT INTO LedgerHeaders (ledgerHash, prevHash, txSetHash, clfHash, totCoins, "\
            "feePool, ledgerSeq,inflationSeq,baseFee,baseReserve,closeTime) VALUES"\
            "(:h,:ph,:tx,:clf,:coins,"\
            ":feeP,:seq,:inf,:Bfee,:res,"\
            ":ct )",
            use(hash), use(prevHash),
            use(txSetHash), use(clfHash),
            use(mHeader.totalCoins), use(mHeader.feePool), use(mHeader.ledgerSeq),
            use(mHeader.inflationSeq), use(mHeader.baseFee), use(mHeader.baseReserve),
            use(mHeader.closeTime));
        {
            auto timer = db.getInsertTimer("ledger-header");
            st.execute(true);
        }
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadByHash(Hash const& hash, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        LedgerHeader lh;

        string hash_s(binToHex(hash));
        string prevHash, txSetHash, clfHash;
        {
            auto& db = ledgerMaster.getDatabase();
            auto timer = db.getSelectTimer("ledger-header");
            db.getSession() <<
                "SELECT prevHash, txSetHash, clfHash, totCoins, "   \
                "feePool, ledgerSeq,inflationSeq,baseFee,"          \
                "baseReserve,closeTime FROM LedgerHeaders "         \
                "WHERE ledgerHash = :h",
                into(prevHash), into(txSetHash), into(clfHash), into(lh.totalCoins),
                into(lh.feePool), into(lh.ledgerSeq), into(lh.inflationSeq), into(lh.baseFee),
                into(lh.baseReserve), into(lh.closeTime),
                use(hash_s);
        }
        if (ledgerMaster.getDatabase().getSession().got_data())
        {
            lh.previousLedgerHash = hexToBin256(prevHash);
            lh.txSetHash = hexToBin256(txSetHash);
            lh.clfHash = hexToBin256(clfHash);

            lhf = make_shared<LedgerHeaderFrame>(lh);
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

        LedgerHeader lh;

        string hash, prevHash, txSetHash, clfHash;
        auto& db = ledgerMaster.getDatabase();

        {
            auto timer = db.getSelectTimer("ledger-header");
            db.getSession() <<
            "SELECT ledgerHash, prevHash, txSetHash, clfHash, totCoins, "\
            "feePool, inflationSeq,baseFee,"\
            "baseReserve,closeTime FROM LedgerHeaders "\
            "WHERE ledgerSeq = :s",
            into(hash), into(prevHash), into(txSetHash), into(clfHash), into(lh.totalCoins),
            into(lh.feePool), into(lh.inflationSeq), into(lh.baseFee),
            into(lh.baseReserve), into(lh.closeTime),
            use(seq);
        }
        if (ledgerMaster.getDatabase().getSession().got_data())
        {
            lh.ledgerSeq = seq;
            lh.previousLedgerHash = hexToBin256(prevHash);
            lh.txSetHash = hexToBin256(txSetHash);
            lh.clfHash = hexToBin256(clfHash);

            lhf = make_shared<LedgerHeaderFrame>(lh);
            if (lhf->getHash() != hexToBin256(hash))
            {
                // wrong hash
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

        LedgerHeaderHistoryEntry lhe;
        LedgerHeader &lh = lhe.header;
        string hash, prevHash, txSetHash, clfHash;

        assert(begin <= end);

        soci::statement st =
            (sess.prepare <<
             "SELECT ledgerSeq, ledgerHash, prevHash, txSetHash, clfHash, totCoins, " \
             "feePool, inflationSeq, baseFee,"                           \
             "baseReserve, closeTime FROM LedgerHeaders "                \
             "WHERE ledgerSeq >= :begin AND ledgerSeq < :end",
             into(lh.ledgerSeq), into(hash), into(prevHash), into(txSetHash), into(clfHash),
             into(lh.totalCoins), into(lh.feePool), into(lh.inflationSeq), into(lh.baseFee),
             into(lh.baseReserve), into(lh.closeTime),
             use(begin), use(end));

        st.execute(true);
        while (st.got_data())
        {
            lhe.hash = hexToBin256(hash);
            lh.previousLedgerHash = hexToBin256(prevHash);
            lh.txSetHash = hexToBin256(txSetHash);
            lh.clfHash = hexToBin256(clfHash);
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
            "txSetHash       CHARACTER(64) NOT NULL,"
            "clfHash         CHARACTER(64) NOT NULL,"
            "totCoins        BIGINT NOT NULL CHECK (totCoins >= 0),"
            "feePool         BIGINT NOT NULL CHECK (feePool >= 0),"
            "ledgerSeq       BIGINT UNIQUE CHECK (ledgerSeq >= 0),"
            "inflationSeq    INT NOT NULL CHECK (inflationSeq >= 0),"
            "baseFee         INT NOT NULL CHECK (baseFee >= 0),"
            "baseReserve     INT NOT NULL CHECK (baseReserve >= 0),"
            "closeTime       BIGINT NOT NULL CHECK (closeTime >= 0)"
            ");";

        db.getSession() <<
            "CREATE INDEX LedgersBySeq ON LedgerHeaders ( ledgerSeq );";
    }
}

