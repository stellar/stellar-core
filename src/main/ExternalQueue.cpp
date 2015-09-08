// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ExternalQueue.h"

#include "database/Database.h"
#include "Application.h"
#include "ledger/LedgerManager.h"
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
    mApp.getDatabase().getSession() << kSQLCreateStatement;
}

void
ExternalQueue::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS pubsub;";

    soci::statement st = db.getSession().prepare << kSQLCreateStatement;
    st.execute(true);
}

bool
ExternalQueue::validateResourceID(std::string const& resid)
{
    static std::regex re("^[A-Z][A-Z0-9]{0,31}$");
    return std::regex_match(resid, re);
}

void
ExternalQueue::setCursorForResource(std::string const& resid, uint32 cursor)
{
    checkID(resid);

    std::string old(getCursor(resid));
    if (old.empty())
    {
        auto timer = mApp.getDatabase().getInsertTimer("pubsub");
        auto prep = mApp.getDatabase().getPreparedStatement(
            "INSERT INTO pubsub (resid, lastread) VALUES (:n, :v);");
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
            "UPDATE pubsub SET lastread = :v WHERE resid = :n;");

        auto& st = prep.statement();
        st.exchange(soci::use(cursor));
        st.exchange(soci::use(resid));
        st.define_and_bind();
        {
            auto timer = mApp.getDatabase().getUpdateTimer("pubsub");
            st.execute(true);
        }
    }
}

void
ExternalQueue::deleteCursor(std::string const& resid)
{
    checkID(resid);

    auto timer = mApp.getDatabase().getInsertTimer("pubsub");
    auto prep = mApp.getDatabase().getPreparedStatement(
        "DELETE FROM pubsub WHERE resid = :n;");
    auto& st = prep.statement();
    st.exchange(soci::use(resid));
    st.define_and_bind();
    st.execute(true);
}

void
ExternalQueue::process()
{
    auto& db = mApp.getDatabase();
    int m;
    soci::indicator minIndicator;
    soci::statement st =
        (db.getSession().prepare << "SELECT MIN(lastread) FROM pubsub",
         soci::into(m, minIndicator));
    {
        auto timer = db.getSelectTimer("state");
        st.execute(true);
    }

    if (st.got_data() && minIndicator == soci::indicator::i_ok)
    {
        mApp.getLedgerManager().deleteOldEntries(mApp.getDatabase(), (uint32)m);
    }
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
    checkID(resid);
    std::string res;

    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(
        "SELECT lastread FROM pubsub WHERE resid = :n;");
    auto& st = prep.statement();
    st.exchange(soci::into(res));
    st.exchange(soci::use(resid));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("pubsub");
        st.execute(true);
    }

    if (!st.got_data())
    {
        res.clear();
    }

    return res;
}
}