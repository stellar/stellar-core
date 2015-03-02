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

    LedgerHeaderFrame::LedgerHeaderFrame(LedgerHeader lh) : mHeader(lh)
    {

    }

    LedgerHeaderFrame::LedgerHeaderFrame(LedgerHeaderFrame::pointer lastClosedLedger) :
        mHeader(lastClosedLedger->mHeader)
    {
        mHeader.hash.fill(0); // hashes are invalid and need to be recomputed
        mHeader.ledgerSeq++;
        mHeader.previousLedgerHash = lastClosedLedger->mHeader.hash;
    }

    void LedgerHeaderFrame::computeHash()
    {
        if (isZero(mHeader.hash))
        {
            // Hash is hash(header-with-hash-field-all-zero)
            mHeader.hash = sha256(xdr::xdr_to_msg(mHeader));
            assert(!isZero(mHeader.hash));
        }
    }

    void LedgerHeaderFrame::storeInsert(LedgerMaster& ledgerMaster)
    {
        assert(!isZero(mHeader.hash));

        string hash(binToHex(mHeader.hash)), prevHash(binToHex(mHeader.previousLedgerHash)),
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

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadByHash(const uint256 &hash, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        lhf = make_shared<LedgerHeaderFrame>();

        LedgerHeader &lh = lhf->mHeader;

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
            lh.hash = hash;
            lh.previousLedgerHash = hexToBin256(prevHash);
            lh.txSetHash = hexToBin256(txSetHash);
            lh.clfHash = hexToBin256(clfHash);
        }
        else
        {
            lhf = LedgerHeaderFrame::pointer();
        }

        return lhf;
    }

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadBySequence(uint64_t seq, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        lhf = make_shared<LedgerHeaderFrame>();

        LedgerHeader &lh = lhf->mHeader;

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
            lh.hash = hexToBin256(hash);
            lh.previousLedgerHash = hexToBin256(prevHash);
            lh.txSetHash = hexToBin256(txSetHash);
            lh.clfHash = hexToBin256(clfHash);
        }
        else
        {
            lhf = LedgerHeaderFrame::pointer();
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

        LedgerHeader lh;
        string hash, prevHash, txSetHash, clfHash;

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
            lh.hash = hexToBin256(hash);
            lh.previousLedgerHash = hexToBin256(prevHash);
            lh.txSetHash = hexToBin256(txSetHash);
            lh.clfHash = hexToBin256(clfHash);
            out.writeOne(lh);
            ++n;
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

