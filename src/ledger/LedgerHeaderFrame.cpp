// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LedgerHeaderFrame.h"
#include "LedgerMaster.h"
#include "lib/json/json.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "database/Database.h"

using namespace soci;

namespace stellar
{

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
            // TODO: compute hash
            mHeader.hash.fill(mHeader.ledgerSeq & 0xFFu);
            // assert(!isZero(mHeader.hash));
        }
    }

    void LedgerHeaderFrame::storeInsert(LedgerMaster& ledgerMaster)
    {
        assert(!isZero(mHeader.hash));

        string hash(binToHex(mHeader.hash)), prevHash(binToHex(mHeader.previousLedgerHash)),
            txSetHash(binToHex(mHeader.txSetHash)), clfHash(binToHex(mHeader.clfHash));

        ledgerMaster.getDatabase().getSession() <<
            "INSERT INTO LedgerHeaders (hash, prevHash, txSetHash, clfHash, totCoins, "\
            "feePool, ledgerSeq,inflationSeq,baseFee,baseReserve,closeTime) VALUES"\
            "(:h,:ph,:tx,:clf,:coins,"\
            ":feeP,:seq,:inf,:Bfee,:res,"\
            ":ct )",
            use(hash), use(prevHash),
            use(txSetHash), use(clfHash),
            use(mHeader.totalCoins), use(mHeader.feePool), use(mHeader.ledgerSeq),
            use(mHeader.inflationSeq), use(mHeader.baseFee), use(mHeader.baseReserve),
            use(mHeader.closeTime);
    }

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadByHash(const uint256 &hash, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        lhf = make_shared<LedgerHeaderFrame>();

        LedgerHeader &lh = lhf->mHeader;

        string hash_s(binToHex(hash));
        string prevHash, txSetHash, clfHash;

        ledgerMaster.getDatabase().getSession() <<
            "SELECT prevHash, txSetHash, clfHash, totCoins, "\
            "feePool, ledgerSeq,inflationSeq,baseFee,"\
            "baseReserve,closeTime FROM LedgerHeaders "\
            "WHERE hash = :h",
            into(prevHash), into(txSetHash), into(clfHash), into(lh.totalCoins),
            into(lh.feePool), into(lh.ledgerSeq), into(lh.inflationSeq), into(lh.baseFee),
            into(lh.baseReserve), into(lh.closeTime),
            use(hash_s);
        lh.hash = hash;
        lh.previousLedgerHash = hexToBin256(prevHash);
        lh.txSetHash = hexToBin256(txSetHash);
        lh.clfHash = hexToBin256(clfHash);

        return lhf;
    }

    LedgerHeaderFrame::pointer LedgerHeaderFrame::loadBySequence(uint64_t seq, LedgerMaster& ledgerMaster)
    {
        LedgerHeaderFrame::pointer lhf;

        lhf = make_shared<LedgerHeaderFrame>();
        
        LedgerHeader &lh = lhf->mHeader;

        string hash, prevHash, txSetHash, clfHash;

        ledgerMaster.getDatabase().getSession() <<
            "SELECT hash, prevHash, txSetHash, clfHash, totCoins, "\
            "feePool, inflationSeq,baseFee,"\
            "baseReserve,closeTime FROM LedgerHeaders "\
            "WHERE ledgerSeq = :s",
            into(hash), into(prevHash), into(txSetHash), into(clfHash), into(lh.totalCoins),
            into(lh.feePool), into(lh.inflationSeq), into(lh.baseFee),
            into(lh.baseReserve), into(lh.closeTime),
            use(seq);
        lh.ledgerSeq = seq;
        lh.hash = hexToBin256(hash);
        lh.previousLedgerHash = hexToBin256(prevHash);
        lh.txSetHash = hexToBin256(txSetHash);
        lh.clfHash = hexToBin256(clfHash);

        return lhf;
    }

    void LedgerHeaderFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS LedgerHeaders;";

        db.getSession() <<
            "CREATE TABLE IF NOT EXISTS LedgerHeaders ("\
            "hash            CHARACTER(64) PRIMARY KEY,"\
            "prevHash        CHARACTER(64) NOT NULL,"\
            "txSetHash       CHARACTER(64) NOT NULL,"\
            "clfHash         CHARACTER(64) NOT NULL,"\
            "totCoins        BIGINT UNSIGNED NOT NULL,"\
            "feePool         BIGINT UNSIGNED NOT NULL,"\
            "ledgerSeq       BIGINT UNSIGNED UNIQUE,"\
            "inflationSeq    INT UNSIGNED NOT NULL,"\
            "baseFee         INT UNSIGNED NOT NULL,"\
            "baseReserve     INT UNSIGNED NOT NULL,"\
            "closeTime       BIGINT UNSIGNED NOT NULL"\
            ");";

        db.getSession() <<
            "CREATE INDEX IF NOT EXISTS ledgersBySeq ON LedgerHeaders ( ledgerSeq );";
    }
}

