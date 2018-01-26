// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerState.h"
#include "util/basen.h"

namespace stellar
{

LedgerState::StateEntry
LedgerState::loadDataFromDatabase(LedgerKey const& key)
{
    auto& db = mRoot->getDatabase();

    std::string actIDStrKey = KeyUtils::toStrKey(key.data().accountID);
    std::string const& dataName = key.data().dataName;

    std::string dataValue;
    soci::indicator dataValueIndicator;

    LedgerEntry le;
    le.data.type(DATA);
    DataEntry& de = le.data.data();

    std::string sql = "SELECT datavalue, lastmodified "
                      "FROM accountdata "
                      "WHERE accountid= :id AND dataname= :dataname";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(dataValue, dataValueIndicator));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.define_and_bind();
    st.execute(true);
    if (!st.got_data())
    {
        return nullptr;
    }

    de.accountID = key.data().accountID;
    de.dataName = dataName;

    if (dataValueIndicator != soci::i_ok)
    {
        throw std::runtime_error("bad database state");
    }
    bn::decode_b64(dataValue, de.dataValue);

    auto entry = std::make_shared<LedgerEntry>(std::move(le));
    return getLeafLedgerState().makeStateEntry(entry, entry);
}

void
LedgerState::storeDataInDatabase(StateEntry const& state)
{
    bool isInsert = !state->ignoreInvalid().previousEntry();
    auto& db = mRoot->getDatabase();

    auto const& entry = *state->ignoreInvalid().entry();
    auto const& data = entry.data.data();
    std::string actIDStrKey = KeyUtils::toStrKey(data.accountID);
    std::string const& dataName = data.dataName;
    std::string dataValue = bn::encode_b64(data.dataValue);

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO accountdata "
              "(accountid,dataname,datavalue,lastmodified)"
              " VALUES (:aid,:dn,:dv,:lm)";
    }
    else
    {
        sql = "UPDATE accountdata SET datavalue=:dv,lastmodified=:lm "
              " WHERE accountid=:aid AND dataname=:dn";
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "aid"));
    st.exchange(soci::use(dataName, "dn"));
    st.exchange(soci::use(dataValue, "dv"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "lm"));
    st.define_and_bind();
    st.execute(true);
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
}

void
LedgerState::deleteDataFromDatabase(StateEntry const& state)
{
    auto& db = mRoot->getDatabase();

    auto const& previousEntry = state->ignoreInvalid().previousEntry();
    auto const& data = previousEntry->data.data();
    std::string actIDStrKey = KeyUtils::toStrKey(data.accountID);
    std::string const& dataName = data.dataName;

    // TODO(jonjove): Check affected rows?
    // TODO(jonjove): Fix timers?
    auto timer = db.getDeleteTimer("data");
    auto prep = db.getPreparedStatement(
        "DELETE FROM accountdata WHERE accountid=:id AND dataname=:s");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.define_and_bind();
    st.execute(true);
}
}
