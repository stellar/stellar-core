// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "DataQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "util/basen.h"

namespace stellar
{

namespace
{
const auto DROP_TABLE = "DROP TABLE IF EXISTS accountdata;";

const auto CREATE_TABLE = "CREATE TABLE accountdata"
                          "("
                          "accountid    VARCHAR(56)  NOT NULL,"
                          "dataname     VARCHAR(64)  NOT NULL,"
                          "datavalue    VARCHAR(112) NOT NULL,"
                          "PRIMARY KEY  (accountid, dataname)"
                          ");";

const auto DATA_COLUMN_SELECTOR =
    "SELECT datavalue,lastmodified FROM accountdata";
}

void
createDataTable(Database& db)
{
    db.getSession() << DROP_TABLE;
    db.getSession() << CREATE_TABLE;
}

std::vector<LedgerEntry>
loadAccountDatas(StatementContext& prep, Database& db)
{
    auto result = std::vector<LedgerEntry>{};
    auto actIDStrKey = std::string{};
    auto dataName = std::string{};
    auto dataValue = std::string{};

    soci::indicator dataNameIndicator, dataValueIndicator;

    LedgerEntry le;
    le.data.type(DATA);
    DataEntry& oe = le.data.data();

    auto& st = prep.statement();
    st.exchange(soci::into(actIDStrKey));
    st.exchange(soci::into(dataName, dataNameIndicator));
    st.exchange(soci::into(dataValue, dataValueIndicator));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        oe.accountID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);

        if ((dataNameIndicator != soci::i_ok) ||
            (dataValueIndicator != soci::i_ok))
        {
            throw std::runtime_error("bad database state");
        }
        oe.dataName = dataName;
        bn::decode_b64(dataValue, oe.dataValue);

        result.push_back(le);
        st.fetch();
    }

    return result;
}

optional<LedgerEntry const>
selectData(AccountID const& accountID, std::string dataName, Database& db)
{
    auto actIDStrKey = KeyUtils::toStrKey(accountID);

    auto sql = std::string{DATA_COLUMN_SELECTOR};
    sql += " WHERE accountid = :id AND dataname = :dataname";
    auto prep = db.getPreparedStatement(sql);

    auto timer = db.getSelectTimer("data");
    auto dataValue = std::string{};

    soci::indicator dataValueIndicator;

    auto result = LedgerEntry{};
    result.data.type(DATA);
    DataEntry& oe = result.data.data();
    oe.accountID = accountID;
    oe.dataName = dataName;

    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.exchange(soci::into(dataValue, dataValueIndicator));
    st.exchange(soci::into(result.lastModifiedLedgerSeq));

    st.define_and_bind();
    st.execute(true);
    if (!st.got_data())
    {
        return nullopt<LedgerEntry>();
    }

    if (dataValueIndicator != soci::i_ok)
    {
        throw std::runtime_error("bad database state");
    }
    oe.dataName = dataName;
    bn::decode_b64(dataValue, oe.dataValue);

    return make_optional<LedgerEntry const>(result);
}

void
insertData(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == DATA);

    auto& data = entry.data.data();
    auto actIDStrKey = KeyUtils::toStrKey(data.accountID);
    auto dataName = std::string{data.dataName};
    auto dataValue = std::string{bn::encode_b64(data.dataValue)};
    auto sql = std::string{"INSERT INTO accountdata "
                           "(accountid,dataname,datavalue,lastmodified)"
                           " VALUES (:aid,:dn,:dv,:lm)"};

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
updateData(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == DATA);

    auto& data = entry.data.data();
    auto actIDStrKey = KeyUtils::toStrKey(data.accountID);
    auto dataName = std::string{data.dataName};
    auto dataValue = std::string{bn::encode_b64(data.dataValue)};
    auto sql =
        std::string{"UPDATE accountdata SET datavalue=:dv,lastmodified=:lm "
                    " WHERE accountid=:aid AND dataname=:dn"};

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

bool
dataExists(LedgerKey const& key, Database& db)
{
    assert(key.type() == DATA);

    auto actIDStrKey = KeyUtils::toStrKey(key.data().accountID);
    auto dataName = std::string{key.data().dataName};
    auto exists = 0;
    auto timer = db.getSelectTimer("data-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM accountdata "
                                "WHERE accountid=:id AND dataname=:s)");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.exchange(soci::into(exists));
    st.define_and_bind();
    st.execute(true);

    return exists != 0;
}

void
deleteData(LedgerKey const& key, Database& db)
{
    assert(key.type() == DATA);

    auto actIDStrKey = KeyUtils::toStrKey(key.data().accountID);
    auto dataName = std::string{key.data().dataName};
    auto timer = db.getDeleteTimer("data");
    auto prep = db.getPreparedStatement(
        "DELETE FROM accountdata WHERE accountid=:id AND dataname=:s");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::use(dataName));
    st.define_and_bind();
    st.execute(true);
}

std::unordered_map<AccountID, int>
selectDataCountPerAccount(Database& db)
{
    auto result = std::unordered_map<AccountID, int>{};
    auto count = 0;
    auto accountId = std::string{};

    auto sql = std::string{R"(
        SELECT COUNT(*), accountid FROM accountdata GROUP BY accountid
    )"};

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(count));
    st.exchange(soci::into(accountId));
    st.define_and_bind();

    auto timer = db.getSelectTimer("data");
    st.execute(true);

    while (st.got_data())
    {
        result.insert(
            std::make_pair(KeyUtils::fromStrKey<PublicKey>(accountId), count));
        st.fetch();
    }

    return result;
}

uint64_t
countData(Database& db)
{
    auto query = std::string{R"(
        SELECT COUNT(*) FROM accountdata
    )"};

    auto result = 0;
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::into(result));
    st.define_and_bind();
    st.execute(true);

    return result;
}
}
