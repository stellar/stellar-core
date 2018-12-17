// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderPersistenceImpl.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "scp/Slot.h"
#include "util/Decoder.h"
#include "util/XDRStream.h"

#include <soci.h>
#include <xdrpp/marshal.h>

namespace stellar
{

std::unique_ptr<HerderPersistence>
HerderPersistence::create(Application& app)
{
    return std::make_unique<HerderPersistenceImpl>(app);
}

HerderPersistenceImpl::HerderPersistenceImpl(Application& app) : mApp(app)
{
}

HerderPersistenceImpl::~HerderPersistenceImpl()
{
}

void
HerderPersistenceImpl::saveSCPHistory(uint32_t seq,
                                      std::vector<SCPEnvelope> const& envs)
{
    if (envs.empty())
    {
        return;
    }

    auto usedQSets = std::unordered_map<Hash, SCPQuorumSetPtr>{};
    auto& db = mApp.getDatabase();

    soci::transaction txscope(db.getSession());

    {
        auto prepClean = db.getPreparedStatement(
            "DELETE FROM scphistory WHERE ledgerseq =:l");

        auto& st = prepClean.statement();
        st.exchange(soci::use(seq));
        st.define_and_bind();
        {
            auto timer = db.getDeleteTimer("scphistory");
            st.execute(true);
        }
    }
    for (auto const& e : envs)
    {
        auto const& qHash =
            Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        usedQSets.insert(
            std::make_pair(qHash, mApp.getHerder().getQSet(qHash)));

        std::string nodeIDStrKey = KeyUtils::toStrKey(e.statement.nodeID);

        auto envelopeBytes(xdr::xdr_to_opaque(e));

        std::string envelopeEncoded;
        envelopeEncoded = decoder::encode_b64(envelopeBytes);

        auto prepEnv =
            db.getPreparedStatement("INSERT INTO scphistory "
                                    "(nodeid, ledgerseq, envelope) VALUES "
                                    "(:n, :l, :e)");

        auto& st = prepEnv.statement();
        st.exchange(soci::use(nodeIDStrKey));
        st.exchange(soci::use(seq));
        st.exchange(soci::use(envelopeEncoded));
        st.define_and_bind();
        {
            auto timer = db.getInsertTimer("scphistory");
            st.execute(true);
        }
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    for (auto const& p : usedQSets)
    {
        std::string qSetH = binToHex(p.first);

        auto prepUpQSet =
            db.getPreparedStatement("UPDATE scpquorums SET "
                                    "lastledgerseq = :l WHERE qsethash = :h");

        auto& stUp = prepUpQSet.statement();
        stUp.exchange(soci::use(seq));
        stUp.exchange(soci::use(qSetH));
        stUp.define_and_bind();
        {
            auto timer = db.getInsertTimer("scpquorums");
            stUp.execute(true);
        }
        if (stUp.get_affected_rows() != 1)
        {
            auto qSetBytes(xdr::xdr_to_opaque(*p.second));

            std::string qSetEncoded;
            qSetEncoded = decoder::encode_b64(qSetBytes);

            auto prepInsQSet = db.getPreparedStatement(
                "INSERT INTO scpquorums "
                "(qsethash, lastledgerseq, qset) VALUES "
                "(:h, :l, :v);");

            auto& stIns = prepInsQSet.statement();
            stIns.exchange(soci::use(qSetH));
            stIns.exchange(soci::use(seq));
            stIns.exchange(soci::use(qSetEncoded));
            stIns.define_and_bind();
            {
                auto timer = db.getInsertTimer("scpquorums");
                stIns.execute(true);
            }
            if (stIns.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
        }
    }

    txscope.commit();
}

size_t
HerderPersistence::copySCPHistoryToStream(Database& db, soci::session& sess,
                                          uint32_t ledgerSeq,
                                          uint32_t ledgerCount,
                                          XDROutputFileStream& scpHistory)
{
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    // all known quorum sets
    std::unordered_map<Hash, SCPQuorumSet> qSets;

    for (uint32_t curLedgerSeq = begin; curLedgerSeq < end; curLedgerSeq++)
    {
        // SCP envelopes for this ledger
        // quorum sets missing in this batch of envelopes
        std::set<Hash> missingQSets;

        SCPHistoryEntry hEntryV;
        hEntryV.v(0);
        auto& hEntry = hEntryV.v0();
        auto& lm = hEntry.ledgerMessages;
        lm.ledgerSeq = curLedgerSeq;

        auto& curEnvs = lm.messages;

        // fetch SCP messages from history
        {
            std::string envB64;

            auto timer = db.getSelectTimer("scphistory");

            soci::statement st =
                (sess.prepare << "SELECT envelope FROM scphistory "
                                 "WHERE ledgerseq = :cur ORDER BY nodeid",
                 soci::into(envB64), soci::use(curLedgerSeq));

            st.execute(true);

            while (st.got_data())
            {
                curEnvs.emplace_back();
                auto& env = curEnvs.back();

                std::vector<uint8_t> envBytes;
                decoder::decode_b64(envB64, envBytes);

                xdr::xdr_get g1(&envBytes.front(), &envBytes.back() + 1);
                xdr_argpack_archive(g1, env);

                // record new quorum sets encountered
                Hash const& qSetHash =
                    Slot::getCompanionQuorumSetHashFromStatement(env.statement);
                if (qSets.find(qSetHash) == qSets.end())
                {
                    missingQSets.insert(qSetHash);
                }

                n++;

                st.fetch();
            }
        }

        // fetch the quorum sets from the db
        for (auto const& q : missingQSets)
        {
            std::string qset64, qSetHashHex;

            hEntry.quorumSets.emplace_back();
            auto& qset = hEntry.quorumSets.back();

            qSetHashHex = binToHex(q);

            auto timer = db.getSelectTimer("scpquorums");

            soci::statement st = (sess.prepare << "SELECT qset FROM scpquorums "
                                                  "WHERE qsethash = :h",
                                  soci::into(qset64), soci::use(qSetHashHex));

            st.execute(true);

            if (!st.got_data())
            {
                throw std::runtime_error(
                    "corrupt database state: missing quorum set");
            }

            std::vector<uint8_t> qSetBytes;
            decoder::decode_b64(qset64, qSetBytes);

            xdr::xdr_get g1(&qSetBytes.front(), &qSetBytes.back() + 1);
            xdr_argpack_archive(g1, qset);
        }

        if (curEnvs.size() != 0)
        {
            scpHistory.writeOne(hEntryV);
        }
    }

    return n;
}

void
HerderPersistence::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS scphistory";

    db.getSession() << "DROP TABLE IF EXISTS scpquorums";

    db.getSession() << "CREATE TABLE scphistory ("
                       "nodeid      CHARACTER(56) NOT NULL,"
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "envelope    TEXT NOT NULL"
                       ")";

    db.getSession() << "CREATE INDEX scpenvsbyseq ON scphistory(ledgerseq)";

    db.getSession() << "CREATE TABLE scpquorums ("
                       "qsethash      CHARACTER(64) NOT NULL,"
                       "lastledgerseq INT NOT NULL CHECK (lastledgerseq >= 0),"
                       "qset          TEXT NOT NULL,"
                       "PRIMARY KEY (qsethash)"
                       ")";

    db.getSession()
        << "CREATE INDEX scpquorumsbyseq ON scpquorums(lastledgerseq)";
}

void
HerderPersistence::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "scphistory", "ledgerseq");
    DatabaseUtils::deleteOldEntriesHelper(db.getSession(), ledgerSeq, count,
                                          "scpquorums", "lastledgerseq");
}
}
