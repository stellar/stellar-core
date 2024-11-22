// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ExternalQueue.h"

#include "Application.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <limits>
#include <regex>

namespace stellar
{

using namespace std;

string ExternalQueue::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS pubsub ("
    "resid       CHARACTER(32) PRIMARY KEY,"
    "lastread    INTEGER"
    "); ";

ExternalQueue::ExternalQueue(Application& app) : mApp(app)
{
}

void
ExternalQueue::dropAll(Database& db)
{
    db.getRawMiscSession() << "DROP TABLE IF EXISTS pubsub;";

    soci::statement st = db.getRawMiscSession().prepare << kSQLCreateStatement;
    st.execute(true);
}

bool
ExternalQueue::validateResourceID(std::string const& resid)
{
    static std::regex re("^[A-Z][A-Z0-9]{0,31}$");
    return std::regex_match(resid, re);
}

void
ExternalQueue::setInitialCursors(std::vector<std::string> const& initialResids)
{
    for (auto const& resid : initialResids)
    {
        addCursorForResource(resid, 1);
    }
}

void
ExternalQueue::addCursorForResource(std::string const& resid, uint32 cursor)
{
    ZoneScoped;
    if (getCursor(resid).empty())
    {
        setCursorForResource(resid, cursor);
    }
}

void
ExternalQueue::setCursorForResource(std::string const& resid, uint32 cursor)
{
    ZoneScoped;
    checkID(resid);

    std::string old(getCursor(resid));
    if (old.empty())
    {
        ZoneNamedN(insertPubsubZone, "insert pubsub", true);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "INSERT INTO pubsub (resid, lastread) VALUES (:n, :v);",
            mApp.getDatabase().getMiscSession());
        auto& st = prep.statement();
        st.exchange(soci::use(resid));
        st.exchange(soci::use(cursor));
        st.define_and_bind();
        st.execute(true);
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not insert data in SQL");
        }
    }
    else
    {
        auto prep = mApp.getDatabase().getPreparedStatement(
            "UPDATE pubsub SET lastread = :v WHERE resid = :n;",
            mApp.getDatabase().getMiscSession());

        auto& st = prep.statement();
        st.exchange(soci::use(cursor));
        st.exchange(soci::use(resid));
        st.define_and_bind();
        {
            ZoneNamedN(updatePubsubZone, "update pubsub", true);
            st.execute(true);
        }
    }
}

void
ExternalQueue::getCursorForResource(std::string const& resid,
                                    std::map<std::string, uint32>& curMap)
{
    ZoneScoped;
    // no resid set, get all cursors
    if (resid.empty())
    {
        std::string n;
        uint32_t v;

        auto& db = mApp.getDatabase();
        auto prep =
            db.getPreparedStatement("SELECT resid, lastread FROM pubsub;",
                                    mApp.getDatabase().getMiscSession());
        auto& st = prep.statement();
        st.exchange(soci::into(n));
        st.exchange(soci::into(v));
        st.define_and_bind();
        {
            ZoneNamedN(selectPubsubZone, "select pubsub", true);
            st.execute(true);
        }

        while (st.got_data())
        {
            curMap[n] = v;
            st.fetch();
        }
    }
    else
    {
        // if resid is set attempt to look up the cursor
        // and add it to the map if anything is found
        std::string cursor = getCursor(resid);
        if (!cursor.empty())
        {
            curMap[resid] = strtoul(cursor.c_str(), NULL, 0);
        }
    }
}

void
ExternalQueue::deleteCursor(std::string const& resid)
{
    ZoneScoped;
    checkID(resid);

    {
        ZoneNamedN(deletePubsubZone, "delete pubsub", true);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "DELETE FROM pubsub WHERE resid = :n;",
            mApp.getDatabase().getMiscSession());
        auto& st = prep.statement();
        st.exchange(soci::use(resid));
        st.define_and_bind();
        st.execute(true);
    }
}

void
ExternalQueue::deleteOldEntries(uint32 count)
{
    ZoneScoped;
    auto& db = mApp.getDatabase();
    int m;
    soci::indicator minIndicator;
    soci::statement st = (db.getMiscSession().session().prepare
                              << "SELECT MIN(lastread) FROM pubsub",
                          soci::into(m, minIndicator));
    {
        ZoneNamedN(selectPubsubZone, "select pubsub", true);
        st.execute(true);
    }

    // rmin is the minimum of all last-reads, which means that remote
    // subscribers are ok with us deleting any history N <= rmin.
    // If we do not have subscribers, take this as maxint, and just
    // use the LCL/checkpoint number (see below) to control trimming.
    uint32_t rmin = std::numeric_limits<uint32_t>::max();
    if (st.got_data() && minIndicator == soci::indicator::i_ok)
    {
        rmin = static_cast<uint32_t>(m);
    }

    // Next calculate the minimum of the LCL and/or any queued checkpoint.
    uint32_t lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
    uint32_t ql = mApp.getHistoryManager().getMinLedgerQueuedToPublish();
    uint32_t qmin = ql == 0 ? lcl : std::min(ql, lcl);

    // Next calculate, given qmin, the first ledger it'd be _safe to
    // delete_ while still keeping everything required to publish.
    // So if qmin is (for example) 0x7f = 127, then we want to keep 64
    // ledgers before that, and therefore can erase 0x3f = 63 and less.
    uint32_t freq = mApp.getHistoryManager().getCheckpointFrequency();
    uint32_t lmin = qmin >= freq ? qmin - freq : 0;

    // Cumulative minimum is the lesser of the requirements of history
    // publication and the requirements of our pubsub subscribers.
    uint32_t cmin = std::min(lmin, rmin);

    CLOG_INFO(History,
              "Trimming history <= ledger {} (rmin={}, qmin={}, lmin={})", cmin,
              rmin, qmin, lmin);

    mApp.getLedgerManager().deleteOldEntries(mApp.getDatabase(), cmin, count);
}

void
ExternalQueue::checkID(std::string const& resid)
{
    if (!validateResourceID(resid))
    {
        throw std::invalid_argument("invalid resource ID");
    }
}

std::string
ExternalQueue::getCursor(std::string const& resid)
{
    ZoneScoped;
    checkID(resid);
    std::string res;

    auto& db = mApp.getDatabase();
    auto prep =
        db.getPreparedStatement("SELECT lastread FROM pubsub WHERE resid = :n;",
                                mApp.getDatabase().getMiscSession());
    auto& st = prep.statement();
    st.exchange(soci::into(res));
    st.exchange(soci::use(resid));
    st.define_and_bind();
    {
        ZoneNamedN(selectPubsubZone, "select pubsub", true);
        st.execute(true);
    }

    if (!st.got_data())
    {
        res.clear();
    }

    return res;
}
}
