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
    "databaseinitialized"};

string PersistentState::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS storestate ("
    "statename   CHARACTER(32) PRIMARY KEY,"
    "state       TEXT"
    "); ";

PersistentState::PersistentState(Application& app) : mApp(app)
{
    mApp.getDatabase().getSession() << kSQLCreateStatement;
}

void
PersistentState::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS storestate;";

    soci::statement st = db.getSession().prepare << kSQLCreateStatement;
    st.execute(true);

    soci::statement st2 =
        db.getSession().prepare
        << "INSERT INTO storestate (statename, state) VALUES ('" + mapping[kDatabaseInitialized] + "', 'true');";
    st2.execute(true);
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
    {
        auto timer = db.getSelectTimer("state");
        db.getSession() << "SELECT state FROM storestate WHERE statename = :n;",
            soci::use(sn), soci::into(res);
    }

    if (!mApp.getDatabase().getSession().got_data())
    {
        res.clear();
    }

    return res;
}

void
PersistentState::setState(PersistentState::Entry entry, string const& value)
{
    string sn(getStoreStateName(entry));

    soci::statement st =
        (mApp.getDatabase().getSession().prepare
             << "UPDATE storestate SET state = :v WHERE statename = :n;",
         soci::use(value), soci::use(sn));

    {
        auto timer = mApp.getDatabase().getUpdateTimer("state");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        auto timer = mApp.getDatabase().getInsertTimer("state");
        st = (mApp.getDatabase().getSession().prepare
                  << "INSERT INTO storestate (statename, state) VALUES (:n, :v "
                     ");",
              soci::use(sn), soci::use(value));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not insert data in SQL");
        }
    }
}
}
