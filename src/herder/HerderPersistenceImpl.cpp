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
#include <Tracy.hpp>

#include <optional>
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
                                      std::vector<SCPEnvelope> const& envs,
                                      QuorumTracker::QuorumMap const& qmap)
{
    ZoneScoped;
    if (envs.empty())
    {
        return;
    }

    auto usedQSets = UnorderedMap<Hash, SCPQuorumSetPtr>{};
    auto& db = mApp.getDatabase();
    auto& sess = db.getMiscSession();

    soci::transaction txscope(sess.session());

    {
        auto prepClean = db.getPreparedStatement(
            "DELETE FROM scphistory WHERE ledgerseq =:l", sess);

        auto& st = prepClean.statement();
        st.exchange(soci::use(seq));
        st.define_and_bind();
        {
            ZoneNamedN(deleteSCPHistoryZone, "delete scphistory", true);
            st.execute(true);
        }
    }

    // Prepare multi-row insert into scphistory
    std::vector<std::string> nodeIDs;
    std::vector<uint32_t> seqs;
    std::vector<std::string> envelopes;
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

        nodeIDs.push_back(nodeIDStrKey);
        seqs.push_back(seq);
        envelopes.push_back(envelopeEncoded);
    }

    if (!envs.empty())
    {
        // Perform multi-row insert into scphistory
        auto prepEnv =
            db.getPreparedStatement("INSERT INTO scphistory "
                                    "(nodeid, ledgerseq, envelope) VALUES "
                                    "(:n, :l, :e)",
                                    sess);
        auto& st = prepEnv.statement();
        st.exchange(soci::use(nodeIDs, "n"));
        st.exchange(soci::use(seqs, "l"));
        st.exchange(soci::use(envelopes, "e"));
        st.define_and_bind();
        {
            ZoneNamedN(insertSCPHistoryZone, "insert scphistory", true);
            st.execute(true);
        }
        if (st.get_affected_rows() != envs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    // save quorum information
    for (auto const& p : qmap)
    {
        auto const& nodeID = p.first;
        if (!p.second.mQuorumSet)
        {
            // skip node if we don't have its quorum set
            continue;
        }
        auto qSetH = xdrSha256(*(p.second.mQuorumSet));
        usedQSets.insert(std::make_pair(qSetH, p.second.mQuorumSet));

        std::string nodeIDStrKey = KeyUtils::toStrKey(nodeID);
        std::string qSetHHex(binToHex(qSetH));

        auto prep = db.getPreparedStatement(
            "UPDATE quoruminfo SET qsethash = :h WHERE nodeid = :id", sess);
        auto& st = prep.statement();
        st.exchange(soci::use(qSetHHex));
        st.exchange(soci::use(nodeIDStrKey));
        st.define_and_bind();
        {
            ZoneNamedN(updateQsetZone, "update quoruminfo", true);
            st.execute(true);
        }
        if (st.get_affected_rows() != 1)
        {
            auto prepI = db.getPreparedStatement(
                "INSERT INTO quoruminfo (nodeid, qsethash) VALUES (:id, :h)",
                sess);
            auto& stI = prepI.statement();
            stI.exchange(soci::use(nodeIDStrKey));
            stI.exchange(soci::use(qSetHHex));
            stI.define_and_bind();
            {
                ZoneNamedN(insertQsetZone, "insert quoruminfo", true);
                stI.execute(true);
            }
            if (stI.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
        }
    }
    // save quorum sets
    for (auto const& p : usedQSets)
    {
        std::string qSetH = binToHex(p.first);

        uint32_t lastSeenSeq;
        auto prepSelQSet = db.getPreparedStatement(
            "SELECT lastledgerseq FROM scpquorums WHERE qsethash = :h", sess);
        auto& stSel = prepSelQSet.statement();
        stSel.exchange(soci::into(lastSeenSeq));
        stSel.exchange(soci::use(qSetH));
        stSel.define_and_bind();
        {
            ZoneNamedN(selectSCPQuorumsZone, "select scpquorums", true);
            stSel.execute(true);
        }

        if (stSel.got_data())
        {
            if (lastSeenSeq >= seq)
            {
                continue;
            }

            auto prepUpQSet = db.getPreparedStatement(
                "UPDATE scpquorums SET "
                "lastledgerseq = :l WHERE qsethash = :h",
                sess);

            auto& stUp = prepUpQSet.statement();
            stUp.exchange(soci::use(seq));
            stUp.exchange(soci::use(qSetH));
            stUp.define_and_bind();
            {
                ZoneNamedN(updateSCPQuorumsZone, "update scpquorums", true);
                stUp.execute(true);
            }
            if (stUp.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
        }
        else
        {
            auto qSetBytes(xdr::xdr_to_opaque(*p.second));

            std::string qSetEncoded;
            qSetEncoded = decoder::encode_b64(qSetBytes);

            auto prepInsQSet = db.getPreparedStatement(
                "INSERT INTO scpquorums "
                "(qsethash, lastledgerseq, qset) VALUES "
                "(:h, :l, :v);",
                sess);

            auto& stIns = prepInsQSet.statement();
            stIns.exchange(soci::use(qSetH));
            stIns.exchange(soci::use(seq));
            stIns.exchange(soci::use(qSetEncoded));
            stIns.define_and_bind();
            {
                ZoneNamedN(insertSCPQuorumsZone, "insert scpquorums", true);
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
    ZoneScoped;
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    // all known quorum sets
    UnorderedMap<Hash, SCPQuorumSet> qSets;

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

            ZoneNamedN(selectSCPHistoryZone, "select scphistory", true);

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
                xdr::xdr_from_opaque(envBytes, env);

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

            auto qset = getQuorumSet(db, sess, q);
            if (!qset)
            {
                throw std::runtime_error(
                    "corrupt database state: missing quorum set");
            }
            hEntry.quorumSets.emplace_back(std::move(*qset));
        }

        if (curEnvs.size() != 0)
        {
            scpHistory.writeOne(hEntryV);
        }
    }

    return n;
}

std::optional<Hash>
HerderPersistence::getNodeQuorumSet(Database& db, soci::session& sess,
                                    NodeID const& nodeID)
{
    ZoneScoped;
    std::string nodeIDStrKey = KeyUtils::toStrKey(nodeID);
    std::string qsethHex;

    {
        ZoneNamedN(selectQsetZone, "select quoruminfo", true);
        soci::statement st = (sess.prepare << "SELECT qsethash FROM quoruminfo "
                                              "WHERE nodeid = :id",
                              soci::into(qsethHex), soci::use(nodeIDStrKey));

        st.execute(true);

        std::optional<Hash> res;
        if (st.got_data())
        {
            auto h = hexToBin256(qsethHex);
            res = std::make_optional<Hash>(std::move(h));
        }
        return res;
    }
}

SCPQuorumSetPtr
HerderPersistence::getQuorumSet(Database& db, soci::session& sess,
                                Hash const& qSetHash)
{
    ZoneScoped;
    SCPQuorumSetPtr res;
    SCPQuorumSet qset;
    std::string qset64, qSetHashHex;

    qSetHashHex = binToHex(qSetHash);

    {
        ZoneNamedN(selectSCPQuorumsZone, "select scpquorums", true);
        soci::statement st = (sess.prepare << "SELECT qset FROM scpquorums "
                                              "WHERE qsethash = :h",
                              soci::into(qset64), soci::use(qSetHashHex));

        st.execute(true);

        if (st.got_data())
        {
            std::vector<uint8_t> qSetBytes;
            decoder::decode_b64(qset64, qSetBytes);

            xdr::xdr_get g1(&qSetBytes.front(), &qSetBytes.back() + 1);
            xdr_argpack_archive(g1, qset);

            res = std::make_shared<SCPQuorumSet>(std::move(qset));
        }
        return res;
    }
}

void
HerderPersistence::dropAll(Database& db)
{
    ZoneScoped;
    db.getRawMiscSession() << "DROP TABLE IF EXISTS scphistory";

    db.getRawMiscSession() << "DROP TABLE IF EXISTS scpquorums";

    db.getRawMiscSession() << "CREATE TABLE scphistory ("
                              "nodeid      CHARACTER(56) NOT NULL,"
                              "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                              "envelope    TEXT NOT NULL"
                              ")";

    db.getRawMiscSession()
        << "CREATE INDEX scpenvsbyseq ON scphistory(ledgerseq)";

    db.getRawMiscSession()
        << "CREATE TABLE scpquorums ("
           "qsethash      CHARACTER(64) NOT NULL,"
           "lastledgerseq INT NOT NULL CHECK (lastledgerseq >= 0),"
           "qset          TEXT NOT NULL,"
           "PRIMARY KEY (qsethash)"
           ")";

    db.getRawMiscSession()
        << "CREATE INDEX scpquorumsbyseq ON scpquorums(lastledgerseq)";

    db.getRawMiscSession() << "DROP TABLE IF EXISTS quoruminfo";
    db.getRawMiscSession() << "CREATE TABLE quoruminfo ("
                              "nodeid      CHARACTER(56) NOT NULL,"
                              "qsethash    CHARACTER(64) NOT NULL,"
                              "PRIMARY KEY (nodeid))";
}

void
HerderPersistence::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    ZoneScoped;
    DatabaseUtils::deleteOldEntriesHelper(db.getRawMiscSession(), ledgerSeq,
                                          count, "scphistory", "ledgerseq");
    DatabaseUtils::deleteOldEntriesHelper(db.getRawMiscSession(), ledgerSeq,
                                          count, "scpquorums", "lastledgerseq");
}
}
