// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/Decoder.h"

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadData(LedgerKey const& key) const
{
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
    auto prep = mDatabase.getPreparedStatement(sql);
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
    decoder::decode_b64(dataValue, de.dataValue);

    return std::make_shared<LedgerEntry const>(std::move(le));
}

void
LedgerTxnRoot::Impl::insertOrUpdateData(LedgerEntry const& entry, bool isInsert)
{
    auto const& data = entry.data.data();
    std::string actIDStrKey = KeyUtils::toStrKey(data.accountID);
    std::string const& dataName = data.dataName;
    std::string dataValue = decoder::encode_b64(data.dataValue);

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

    auto prep = mDatabase.getPreparedStatement(sql);
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
LedgerTxnRoot::Impl::deleteData(LedgerKey const& key)
{
    auto const& data = key.data();
    std::string actIDStrKey = KeyUtils::toStrKey(data.accountID);
    std::string const& dataName = data.dataName;

    auto prep = mDatabase.getPreparedStatement(
        "DELETE FROM accountdata WHERE accountid=:id AND dataname=:s");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.define_and_bind();
    {
        auto timer = mDatabase.getDeleteTimer("data");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
LedgerTxnRoot::Impl::dropData()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    mDatabase.getSession() << "DROP TABLE IF EXISTS accountdata;";
    mDatabase.getSession() << "CREATE TABLE accountdata"
                              "("
                              "accountid    VARCHAR(56)  NOT NULL,"
                              "dataname     VARCHAR(64)  NOT NULL,"
                              "datavalue    VARCHAR(112) NOT NULL,"
                              "lastmodified INT          NOT NULL,"
                              "PRIMARY KEY  (accountid, dataname)"
                              ");";
}
}
