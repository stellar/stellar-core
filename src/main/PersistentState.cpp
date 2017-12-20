// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PersistentState.h"

#include "database/Database.h"
#include "util/Logging.h"

namespace stellar
{

using namespace std;

string PersistentState::mapping[kLastEntry] = {
    "lastclosedledger", "historyarchivestate", "forcescponnextlaunch",
    "lastscpdata",      "databaseschema",      "networkpassphrase",
    "ledgerupgrades"};

string PersistentState::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS storestate ("
    "statename   CHARACTER(32) PRIMARY KEY,"
    "state       TEXT"
    "); ";

PersistentState::PersistentState(Application& app) : mApp(app)
{
}

void
PersistentState::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS storestate;";

    soci::statement st = db.getSession().prepare << kSQLCreateStatement;
    st.execute(true);
}

string
PersistentState::getStoreStateName(PersistentState::Entry n)
{
    if (n < 0 || n >= kLastEntry)
    {
        throw out_of_range("unknown entry");
    }
    return mapping[n];
}

string
PersistentState::getState(PersistentState::Entry entry)
{
    string res;

    string sn(getStoreStateName(entry));

    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(
        "SELECT state FROM storestate WHERE statename = :n;");
    auto& st = prep.statement();
    st.exchange(soci::into(res));
    st.exchange(soci::use(sn));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("state");
        st.execute(true);
    }

    if (!st.got_data())
    {
        res.clear();
    }

    return res;
}

void
PersistentState::setState(PersistentState::Entry entry, string const& value)
{
    string sn(getStoreStateName(entry));
    auto prep = mApp.getDatabase().getPreparedStatement(
        "UPDATE storestate SET state = :v WHERE statename = :n;");

    auto& st = prep.statement();
    st.exchange(soci::use(value));
    st.exchange(soci::use(sn));
    st.define_and_bind();
    {
        auto timer = mApp.getDatabase().getUpdateTimer("state");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1 && getState(entry).empty())
    {
        auto timer = mApp.getDatabase().getInsertTimer("state");
        auto prep2 = mApp.getDatabase().getPreparedStatement(
            "INSERT INTO storestate (statename, state) VALUES (:n, :v);");
        auto& st2 = prep2.statement();
        st2.exchange(soci::use(sn));
        st2.exchange(soci::use(value));
        st2.define_and_bind();
        st2.execute(true);
        if (st2.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not insert data in SQL");
        }
    }
}
}
